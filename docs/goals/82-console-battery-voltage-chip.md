# Goal: battery voltage as a live Console chip (issue #82)

**/goal condition (ready to paste):**

```
/goal All acceptance criteria AC1–AC10 in docs/goals/82-console-battery-voltage-chip.md hold, each demonstrated in the transcript by its stated check. The manual checklist at the bottom is explicitly NOT part of this condition.
```

## 1. The goal

Show the battery voltage in the Console page status strip, next to the SD / Wi-Fi / Mode / FW
chips, and have it **actually update** while the page is open.

Issue #82 as originally written asked for a static value read out of the existing
`/check_status` fetch. That part of the issue is now wrong and must be rewritten — see
section 7. Two things changed the design:

1. The Console tab already refreshes several things on its own (SD chip and the recording
   counters every 1.5 s, the event card every ~5 s). A frozen voltage sitting among live
   numbers reads as live and is not. Battery voltage is the number you look at to decide
   "is the engine running", so a stale one invites a wrong conclusion.
2. The dedicated small endpoint you would build for this **already exists** and already
   carries the field. It is `GET /sleep_status`, and the browser already polls it every
   5 s from every tab. Adding `/get_batt_voltage` would be a second copy of it.

So this goal is not "add an endpoint". It is "fix the endpoint we have, then read it".

## 2. Facts the design stands on (verified on `wican-pro` at `8ea8872`)

### The producer

- `sleep_mode_get_voltage()` (`main/sleep_mode.c:2378`) does **not** read the ADC. It is a
  zero-timeout `xQueuePeek` on a 1-deep static queue holding one float, returning
  `ESP_ERR_NOT_FOUND` when nothing has ever been published.
- The producer is `light_sleep_task` (`main/sleep_mode.c:1584`). It reads ADC1 ch3 eight
  times, averages, scales, and publishes through `update_battery_voltage()`
  (`main/sleep_mode.c:859-864`, a bare `xQueueOverwrite`) called at
  `main/sleep_mode.c:1746`.
- Cadence: **500 ms** awake (`VOLTAGE_READ_PERIOD_MS`, `main/sleep_mode.c:633`, delay at
  `:2343`); ~2 s while in the sleep cycle.
- **The linchpin:** the call at `:1746` sits inside `if(ret == ESP_OK)` only — it is
  **outside** the `if (ret == ESP_OK && sleep_en == 1)` gate at `main/sleep_mode.c:1818`.
  And the task itself is created unconditionally: the config guard in `sleep_mode_init()`
  is commented out at `main/sleep_mode.c:2436`. **So `voltage_queue` is live even with
  sleep disabled.**
- `sleep_mode.c` contains **two** implementations behind `#if HARDWARE_VER` — the non-PRO
  copy at `:62-585` and the OBD-PRO copy at `:586-2481`. `CMakeLists.txt:54` sets
  `HARDWARE_VER=${WICAN_PRO}`, so **every anchor above is in the PRO branch**. Do not edit
  the dead one.

### The endpoint we already have

- `GET /sleep_status` (`main/config_server.c:2432-2463`) is a `char body[160]` stack buffer
  plus one non-blocking queue peek. No cJSON, no heap. It already emits
  `{"state":...,"secs_left":N,"secs_total":N,"voltage":%.2f}` (`:2456-2458`).
- Its doc comment at `main/config_server.c:2409-2412` records why it exists rather than
  being folded into `/check_status`: *"that response is ~2 KB, rebuilds a dozen subsystem
  strings and carries (redacted) WiFi credentials, and the banner has to poll from EVERY
  tab every couple of seconds. This one is a stack buffer and a single non-blocking queue
  peek."* This is one endpoint's design note, **not** a repo-wide rule — but it is the
  right precedent, and its reasoning applies here unchanged.
- **Its one defect:** it sources voltage from `info.voltage` (`main/config_server.c:2441`),
  which comes from `sleep_state_queue`. Every publisher of that queue is behind the
  `sleep_en == 1` gate — the loop publish at `main/sleep_mode.c:2025` (inside `:1818`), the
  teardown publish at `:1179`, and the resume publish at `:1463`. **With sleep disabled,
  `sleep_mode_get_state()` returns `ESP_ERR_NOT_FOUND` and the endpoint reports
  `voltage:0.00`.**
- Its doc comment at `main/config_server.c:2430` — *"the reading that started the
  countdown"* — is **stale**. `main/sleep_mode.c:2021-2022` republishes
  `state_info.voltage = battery_voltage` on every loop pass, so with sleep enabled the
  field is already live at 2 Hz.
- The browser polls it every **5000 ms** idle / **2000 ms** during a countdown
  (`SLEEP_POLL_IDLE_MS` / `SLEEP_POLL_FAST_MS`, `main/web/src/main.js:4212-4213`, switched
  at `:4293`), started once from `Load()` at `main/web/src/main.js:4142` and
  **deliberately never stopped** (`main/web/src/main.js:4341-4342`) — it runs on every tab
  for the life of the page, and fires one tick immediately on start.

### Why not the alternatives

| | Requests added | Device cost per poll | Verdict |
|---|---|---|---|
| A. `/check_status` on tab open (issue as written) | 0 | none — the browser already receives that response | Rejected: not live. Also inherits the `"0.00V"` ambiguity below. |
| B. Poll `/check_status` on a timer | 6/min at 10 s | ~2 KB JSON, ~160 heap allocations, two `restart_tracker` reads that CRC32 under a spinlock, an `esp_netif` IPC hop, one `ESP_LOGI` per call (`main/config_server.c:1538`), redacted WiFi credentials in every response | Rejected: heaviest handler in the poll set, on a single-task httpd (`main/config_server.c:3583`) where a slow handler delays OTA upload and CSV download. |
| C. New `GET /batt_voltage` | 12/min at 5 s | ~60 B, one peek | Rejected only as redundant: it would duplicate a float already on the wire every 5 s. URI slots are not scarce (`max_uri_handlers = 48`, `main/config_server.c:3582`, ~32 used), so this is a "don't build a second one" call, not a resource call. |
| **D. Fix `/sleep_status`, ride its existing poll** | **0** | ~70 B, two zero-timeout peeks instead of one | **Chosen.** Also fixes the sleep-disabled hole and the stale comment. |
| E. Voltage in `/csv_status` | 0 | cJSON churn at 1.5 s | Rejected: wrong cadence, and `csv_status_poll_stop()` runs on every tab switch (`main/web/src/main.js:2972`) so there would be no value on any other tab. (Note: `csv_logger` *can* call `sleep_mode_get_voltage()` — it already does at `components/csv_logger/csv_logger.c:446`, with `REQUIRES main`. There is no layering obstacle; the cadence is the reason.) |

### Consumers that constrain us

- `/check_status`'s `batt_voltage` string is read by `main/web/src/main.js:3267` (Status
  page), rendered at `main/web/homepage_full.html:1303`, and used as a bench procedure in
  `.bench-results/crank_capture_plan.md:189`. **Nothing in `tools/`, no tests, no
  components.** It stays exactly as it is.
- `/sleep_status`'s `voltage` has exactly one consumer in the tree:
  `main/web/src/main.js:4326`, `window._sleepVolts = Number(j.voltage) || 0;` — and that
  line sits **inside the `counting` branch**, after the early return at `:4308-4314`. It
  runs only when `state === 'countdown'`.

## 3. The design

### 3.1 Wire format

`/sleep_status`'s `voltage` becomes:

- a bare JSON number with two decimals when a reading exists — `"voltage":12.83`
- bare JSON **`null`** when `sleep_mode_get_voltage()` returns `ESP_ERR_NOT_FOUND`

It is already a bare number today, so the only wire change is `null` replacing a fake
`0.00`, plus the value now being real when sleep is disabled.

This is the first surface on the device that can tell "no reading" from "zero volts".
`/check_status` cannot: it discards the return value at `main/config_server.c:1657`, so
`tmp` stays 0 and the field reads exactly `"0.00V"` for both cases.

**Accepted inconsistency:** after this change the two endpoints disagree about that one
condition — `/sleep_status` says `null`, `/check_status` still says `"0.00V"`. We are not
touching `/check_status`'s field because it is the one with outside consumers. Note it in
the issue rather than fixing it here.

### 3.2 C changes — `main/config_server.c`, `sleep_status_handler` (`:2432`)

- Leave `state` / `secs_left` / `secs_total` exactly as they are, still sourced from
  `sleep_mode_get_state()`. `state` still reports `"off"` when sleep is disabled — that is
  correct and the banner depends on it.
- Delete **both** `float voltage = 0.0f;` (`:2437`) and `voltage = info.voltage;` (`:2441`).
  Deleting only the assignment leaves an unused variable; the compile does not fail
  (`-Wno-error=unused-variable` is on for `main/`) but the repo ratchets its warning count
  in CI (`tools/check_build_budget.py`, run from
  `.github/workflows/build-firmware.yml`), so a new warning is not free.
- Source the voltage separately and render it as a string fragment:

  ```c
  float v = 0.0f;
  char vbuf[16];
  if (sleep_mode_get_voltage(&v) == ESP_OK)
      snprintf(vbuf, sizeof(vbuf), "%.2f", (double)v);
  else
      strlcpy(vbuf, "null", sizeof(vbuf));
  ```

  and change the format string at `:2457` from `\"voltage\":%.2f` to `\"voltage\":%s`,
  passing `vbuf`. `body[160]` has room.
- Update the doc comment: `:2430` becomes the live-reading description, and the header note
  at `:2414-2417` — which currently ties the whole response to the `sleep_en` gate — must
  say that `state` is still gated but `voltage` no longer is.

### 3.3 JS changes — `main/web/src/main.js`

- Add a small renderer, e.g.:

  ```js
  function consoleBattChip(v) {
      var el = document.getElementById('console_chip_batt');
      if (!el) return;
      el.textContent = (typeof v === 'number') ? (v.toFixed(2) + ' V') : '–';
  }
  ```

  `typeof null === 'object'`, so `null` renders the dash and a genuine `0.00` renders
  `0.00 V`.
- **Call site is a trap — get this exactly right.** It goes **immediately after
  `window._sleepFails = 0;` (`main/web/src/main.js:4289`)**, i.e. *before* the
  `var el = sleep_banner_el(); if (!el) return;` guard at `:4290-4291`. Placed after that
  guard, the chip silently stops updating on any page without `#sleep_banner`. It must also
  be before the `counting` branch at `:4292` so it runs in every state.
- **Failure path:** the `.catch` at `:4330-4337` currently never touches the chip, so a
  device that drops off the network would leave the last voltage on screen forever. Add a
  dash there using the counter that already exists:
  `if (window._sleepFails >= SLEEP_LOST_POLLS) consoleBattChip(null);`
  (`SLEEP_LOST_POLLS = 3`, `main/web/src/main.js:4224`).
- `main/web/src/main.js:4326` needs **no** change: `Number(null) || 0` is 0, and the render
  guard at `:4253` (`typeof … === 'number' && … > 0`) already hides the voltage phrase.
- `consoleLoadChips()` (`main/web/src/main.js:4585`) is **not** touched. No `fetch` is added
  anywhere.

### 3.4 HTML — `main/web/homepage_full.html`

Add one chip to the strip at `:1168`, matching the existing markup and starting at the same
en-dash placeholder the others use:

```html
<span class="console-chip">Batt <b id="console_chip_batt">–</b></span>
```

Do **not** seed it with a plausible number. The Status page already makes that mistake —
`<div id="batt_voltage">12.8V&nbsp;</div>` at `:1303` shows a fake 12.8 V until the first
response lands.

### 3.5 Build pipeline

`main/web/src/homepage.html` is **generated**. Edit `main/web/homepage_full.html` and
`main/web/src/main.js`, then run `python tools/build_web.py` and `python tools/lint_web.py`,
and commit the regenerated `main/web/src/homepage.html`.

**Hazard in the lint:** `tools/lint_web.py` check 3 **soft-passes** when `node`/`npx` is
missing — it returns `(True, 'skipped (node/npx unavailable)')`. Since
`main/CMakeLists.txt:97` embeds `web/src/homepage.html` (not `homepage_full.html`), you can
get a green lint with a stale generated file and the chip simply never appears on the
device. Confirm `build_web.py` actually regenerated the file; do not trust lint alone.

## 4. Scope boundaries — what must NOT change

- `/check_status`'s `batt_voltage` field: still the string `"%.2fV"`, still built at
  `main/config_server.c:1655-1661`. It has outside consumers.
- The Status page's `#batt_voltage` display (`main/web/src/main.js:3267`,
  `main/web/homepage_full.html:1303`). Issue #82 explicitly says keep it.
- The sleep-countdown banner's behaviour: the cadence switch (`:4293`), the `"sleeping"`
  goodbye (`:4300-4305`), the hide branch (`:4308-4314`), the minimum-elapsed gate
  (`:4318-4321`) and the connection-lost path (`:4334-4337`) must all behave exactly as
  today. None of them reads `voltage`, so this is a "prove it stayed true" constraint, not
  a change.
- `state` in `/sleep_status` stays gated on `sleep_en` and still reports `"off"` when sleep
  is disabled.
- No new HTTP endpoint. No new `setInterval`. No new `fetch`.
- `consoleLoadChips()` and `consoleRefresh()` are untouched.
- The non-PRO half of `main/sleep_mode.c` (`:62-585`) is dead code on this hardware; do not
  edit it.

## 5. Risks, ranked

1. **JS call placed after the `if (!el) return;` guard** → chip silently never updates, and
   it looks like a firmware bug. Mitigated by AC6, which greps for the ordering.
2. **`build_web.py` not run, lint soft-passes** → chip never appears on the device even
   though everything looks green. Mitigated by AC8.
3. **Stale value on a dead link** → without the `.catch` dash the chip lies during exactly
   the situation where you care. Mitigated by AC5.
4. **Editing the wrong `sleep_mode.c` half.** Mitigated by only touching
   `main/config_server.c` on the C side — `sleep_mode.c` needs no change at all.
5. **`voltage_queue` is last-good-forever.** If the ADC works at boot and then fails
   mid-run, every option — including this one — shows a stale number rather than a dash,
   because the queue is never cleared. Fixing that needs a timestamp beside the float.
   **Out of scope for #82**; record it in the issue.

## 6. Acceptance criteria (each verifiable from the transcript)

- **AC1 — the endpoint no longer reads the gated queue.**
  `grep -n "sleep_mode_get_voltage" main/config_server.c` shows a call inside
  `sleep_status_handler` (line number between the handler start and its closing brace), and
  `grep -n "info.voltage" main/config_server.c` returns **no** matches.

- **AC2 — the unused variable is gone.**
  `grep -n "float voltage = 0.0f;" main/config_server.c` returns no matches.

- **AC3 — the wire format can express "no reading".**
  `grep -n '\\"voltage\\":%s' main/config_server.c` matches the `snprintf` in
  `sleep_status_handler`, and the handler body shown in the transcript contains a branch
  writing the literal `null` on the `!= ESP_OK` path.

- **AC4 — the chip renders a dash, not a fake number, for a non-number.**
  The transcript shows the body of `consoleBattChip` in `main/web/src/main.js` using a
  `typeof … === 'number'` test with `–` on the else branch.

- **AC5 — a dead link blanks the chip.**
  `grep -n "SLEEP_LOST_POLLS" main/web/src/main.js` shows a second use inside the
  `.catch` block of `sleep_status_tick`, calling `consoleBattChip(null)`.

- **AC6 — the call site is before the banner guard.**
  `grep -n "consoleBattChip\|_sleepFails = 0\|if (!el) return" main/web/src/main.js` shows
  the `consoleBattChip(` call on a line **greater than** the `window._sleepFails = 0;` line
  and **less than** the `if (!el) return;` line that follows it in `sleep_status_tick`.

- **AC7 — nothing new is polled.**
  `grep -c "setInterval" main/web/src/main.js` returns the same count as on `wican-pro`
  (5), `grep -n "fetch('/check_status')" main/web/src/main.js` still returns exactly one
  match, and `grep -c "httpd_register_uri_handler" main/config_server.c` is unchanged.

- **AC8 — the generated page really was regenerated.**
  `python tools/build_web.py` runs and exits 0, and afterwards
  `grep -c "console_chip_batt" main/web/src/homepage.html` returns at least 1.
  `git status --short main/web/src/homepage.html` shows the file as modified.

- **AC9 — lint and web tests pass.**
  `python tools/lint_web.py` exits 0, and `node --test tools/webtest/*.test.mjs` exits 0.

- **AC10 — the firmware builds.**
  `idf.py build` exits 0, and `python tools/check_build_budget.py --log <build log>` exits 0
  (no new warnings against `tools/build_baseline.json`).

## 7. Issue #82 text that must change

The current acceptance checklist is now wrong in two places:

- *"A static value (refreshed when the tab opens) is enough. Live updating is not required."*
  → replace with: the chip updates on the existing `/sleep_status` poll (5 s idle, 2 s
  during a sleep countdown) and shows a dash when there is no reading.
- *"The value comes from the existing `/check_status` fetch in `consoleLoadChips()` — no
  extra HTTP request added."*
  → replace with: the value comes from the existing `/sleep_status` poll. No extra HTTP
  request is added and `consoleLoadChips()` is not modified.
- The line *"reads `sleep_mode_get_voltage()` and adds `batt_voltage` as a string, formatted
  `"%.1fV"`"* is stale — it is `"%.2fV"` (`main/config_server.c:1659`).

Add a note recording the out-of-scope item from section 5.5 (last-good-forever voltage).

## 8. Related, explicitly OUT of scope — do not fix in this PR

Both were found while investigating this issue. Neither belongs here; both deserve their
own ticket.

1. **Format-string bug on the config-save path.** `main/config_server.c:4120` does
   `fprintf(f, resp_str);` — the config JSON, which contains the user's real WiFi SSID and
   password, is passed as the **format string**. A stored value containing `%s` reads a
   garbage stack pointer into `config.json`; `%n` writes through one. The function then
   calls `config_server_schedule_reboot(...)`, so the device restarts into whatever was
   written. Fix is `fprintf(f, "%s", resp_str);`. The correct idiom is already used at
   `main/sdcard.c:473` and `components/event_log/event_log.c:388`; this is the only bad
   call site in the repo.

2. **Missing NULL check in `check_status_handler`.** `cJSON_PrintUnformatted()`
   (`main/config_server.c:1675`) can return NULL and the result goes straight into
   `httpd_resp_send(..., HTTPD_RESP_USE_STRLEN)` at `:1688` → `strlen(NULL)` → panic.
   Low probability: with `CONFIG_SPIRAM=y`, 8 MB PSRAM and
   `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=128`, IDF's `heap_caps_malloc_default`
   (`heap_caps.c:117-126`) falls back across both pools, so this needs internal RAM **and**
   8 MB of PSRAM exhausted at the same moment. The in-file precedent for the fix is
   `main/config_server.c:1173-1179`.

## 9. Manual checklist — OUTSIDE the /goal condition

None of these can be shown in the session transcript; they need the firmware flashed onto
the bench device, which **is not part of this goal**.

- [ ] Flash the branch to the bench device and open the Console tab. The Batt chip shows a
      real voltage within ~5 s, next to SD / Wi-Fi / Mode / FW.
- [ ] Leave the Console tab open and change the supply voltage. The chip follows within
      ~5 s **without** touching the page. (This is the behaviour the whole change exists
      for.)
- [ ] Switch to the Settings tab and back. The chip is still current, not blank and not
      stale.
- [ ] **Set `sleep_status` to `disable` in the config, reboot, and confirm the chip still
      shows a real voltage.** This is the specific hole being fixed; with the old firmware
      it reads `0.00`.
- [ ] Set `sleep_status` back to `enable`, force a countdown, and confirm the sleep banner
      still shows its `— battery low (12.34 V).` phrase and still counts down normally.
- [ ] Pull power / let the device sleep. After ~3 failed polls the chip goes to a dash
      rather than freezing on the last number.
- [ ] Confirm the Status page still shows its own battery voltage, unchanged.

**Merge gate:** per the project rule, nothing merges to `wican-pro` until the bench checks
above are done on hardware. The bench device was in use by another session when this
document was written.
