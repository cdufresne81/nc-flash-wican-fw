# Wi-Fi diagnostics — what the device records about its own link

Issue #105. Users report two symptoms in station mode — transfers that are slow, and connections
that drop — and before this component the firmware kept no evidence of either.

The reason that mattered most: the IDF hands us a disconnect reason code on every drop, and
`wifi_mgr.c` logged it with `ESP_LOGW` and threw it away. This build compiles at
`CONFIG_LOG_MAXIMUM_LEVEL=INFO`, and the board's USB-C port is a USB *host* at runtime, so there is
no serial console anyone can read. By the time a user noticed, the single most useful fact about the
failure was gone.

`components/wifi_diag/` keeps that evidence and presents it three ways: a live page, a paste-ready
report, and a persistent trail that survives reboots.

## The two data sources, and why both

The symptoms fail differently, so they are measured differently.

| Source | What it produces | Lives in |
|---|---|---|
| 1 Hz sampler task (`wd_sampler_task`) | RSSI, channel, PHY, power-save mode, AP/STA channel clash, internal-heap headroom → rolling aggregates + a 120-sample RSSI ring | RAM only |
| Event hooks (`wifi_diag_note_*`) | every association, IP lease, disconnect-with-reason, SSID ban | RAM ring **and** the SD event log |

A single spot reading proves nothing — *"RSSI averaged −78 dBm over twenty minutes"* is a diagnosis
and *"RSSI is −78 dBm"* is not. Equally, the aggregates alone cannot explain a drop; only the reason
code can, and only if it was captured at the moment it happened.

The RSSI recorded against a disconnect is the sampler's **last reading while the link was up**.
`esp_wifi_sta_get_ap_info()` fails the instant the link drops, so reading it inside the disconnect
handler would report nothing at all. This is the main reason the two halves are coupled rather than
independent.

## Persistence is delegated, not reimplemented

Milestone lines are emitted as `EVL_WIFI` through [event_log](../../components/event_log/include/event_log.h),
which already ships a brick-safe SD writer, an RTC crash guard, rotation, and a retrieval endpoint.
Reimplementing that would have meant a second writer task and a second crash guard for the same job.

It also puts Wi-Fi drops in the **same timeline** as boot, sleep and datalog events, which is what
the drop-while-driving case needs — nobody is holding a browser open in a moving car, and the
question is almost always *"what else was happening when it dropped?"*

The 1 Hz samples are deliberately **not** persisted. At ~86k lines a day they would rotate the rest
of the log away, and their whole value is in the aggregate.

### Rate limiting

A device stuck in a reconnect loop can drop every few seconds. `WD_EVL_REPEAT_MS` (60 s) throttles
only the *shared log line* for a repeating identical reason. The RAM ring and the per-reason tally
still record **every** drop, and the suppressed count rides along on the next line that does get
through (`[+N similar suppressed]`), so nothing is silently lost.

## Findings — the part that makes it a tool

`wd_findings()` reads the aggregates against a fixed rule set and states what was observed *and what
it means for the symptom*. This is what separates the page from a data dump. The rules are
deliberately conservative: a missed diagnosis is recoverable, a confident wrong one sends someone
off for a day.

The rules cover power save, AP/STA channel clash, average RSSI, PHY rate (802.11b-only, no 802.11n),
a dominant disconnect reason, internal-RAM headroom, link-uptime percentage, and slow connects.

**A rule that always fires is worse than no rule.** The internal-RAM rule originally triggered below
a 32 KB largest contiguous block. A healthy unit in the field runs with roughly **30 KB of internal
RAM free in total**, so that threshold was arithmetically unreachable and the finding appeared on
every report of every device — which does not warn anyone, it just teaches them to skip the findings
panel. It is now `WD_LOW_BLOCK_BYTES` (4096), calibrated to what the driver needs (a Wi-Fi TX buffer
is ~1.6 KB). Retune it against a measured device, never against intuition, and keep the page's red
threshold equal to it — a host test pins the two together.

Two of them exist because the code already told us where to look:

- **Power save.** `wifi_network.c` carries a comment stating that the SmartConnect path never calls
  `esp_wifi_set_ps()` and so runs on the IDF station default (`WIFI_PS_MIN_MODEM`), while manual
  setup gets `WIFI_PS_NONE` via `wifi_mgr_init()`. Same hardware, same router, two different
  power-save regimes depending on how the user set Wi-Fi up. `wd_findings()` reports the mode the
  radio is *actually* in, read back with `esp_wifi_get_ps()`.
- **AP/STA channel clash.** The device runs AP+STA by default. One radio cannot hold two channels;
  it time-slices. `wifi_mgr.h` already defines `WIFI_STA_AP_OVERLAP_BIT` for this condition — the
  sampler measures how much of the time it is actually true.

The TCP window (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT`) is reported as a **fact in the report's
stack-configuration section, not as a finding**. It is not a fault, but it is a hard ceiling on
single-stream throughput — roughly window ÷ round-trip-time — and it is worth knowing when a
transfer is slow while signal, channel and power save all look healthy. Findings stay observations;
this is a constant.

> **Read the value from the right file.** This build uses the **committed `sdkconfig`**, where the
> window is **20480** bytes (~8.2 Mbit/s at 20 ms RTT). It is *not* `sdkconfig.esp32s3`, which
> carries the IDF default of 5744 and is not what the workflow builds — the build step says "uses
> the committed sdkconfig as-is". An early version of this feature took the number from the wrong
> file, concluded the window was a likely cause of slow transfers, and printed a hardcoded
> "about 2 Mbit/s" sentence directly underneath the correct figure. The report now computes that
> sentence from the macro so the two can never disagree again.

## Routes

All four are registered by `wifi_diag_register_handlers()`, called from `config_server.c` **before**
the catch-all wildcard handler (order is load-bearing — the wildcard would swallow them).

| Route | Serves |
|---|---|
| `/wifi_diag` | JSON: snapshot, aggregates, tally, findings, event ring. Feeds the page. |
| `/wifi_diag/page` | The standalone diagnostic page |
| `/wifi_diag/report` | Plain-text investigation report (`?raw=1` to unmask) |

### Masking

The **report masks** the SSID (first and last character, plus the true length — a length mismatch is
how a trailing-space or homoglyph SSID typo gets spotted) and the tail of the BSSID (the OUI stays:
it identifies the AP vendor, which is diagnostic, and is not specific to a household). Its entire
purpose is to be pasted into a public issue.

The **event log masks too, but without the length.** Its lines are the owner reading his own
device, where he already knows how long his own network name is, so `(N chars)` on every
association line is noise rather than a diagnostic. `wd_mask_ssid_ex(..., with_len=false)` is the
short form and `wd_fact_format()` is its only caller; every other masking call keeps the length.

The **JSON does not mask**. It renders on the user's own device showing their own network, and
masking there would defeat the most common use — spotting that it joined the wrong SSID.

The pre-shared key is never read by this component on any path, so no route can leak it.

**Masking happens at render, never at emit.** Ring entries keep the SSID and BSSID in their own
fields on `wd_evt_t`; the formatted text carries no identifier at all, and `wd_evt_render()` applies
them masked or raw per request. This is structural rather than stylistic: the first version
formatted identifiers straight into the entry text and the report printed the ring verbatim, so
every timeline line published the network name and the router's full MAC beneath a header promising
"masked — safe to paste in public". The failure was invisible at a glance, because the section that
*is* masked sits fifty lines above the one that was not.
`tools/webtest/wifi_diag.test.mjs` now fails if any `wd_evt_push()` format string so much as
mentions an identifier.

## The page

`main/web/wifi_diag.html` is a standalone page, embedded verbatim via `EMBED_FILES` exactly like
`safemode.html`. It is **not** part of the `build_web.py` minify pipeline and **not** scanned by
`lint_web.py` — both own only the single-page app (`homepage_full.html` + `src/main.js`). Edit it
directly; there is no generated counterpart to keep in step.

A separate page rather than a modal, for three reasons that all matter here:

1. The browser's own reload works (there is also an explicit Refresh button and a 5 s auto-refresh).
2. The URL can be bookmarked, or opened on a phone while someone else drives.
3. A page that must stay readable while the Wi-Fi link is failing should not depend on the rest of
   the app having loaded.

When a fetch fails, the page **keeps the last good values on screen** and says they are frozen,
rather than blanking. A failure to load is itself a data point, and blanking at the moment of failure
would destroy the evidence the user opened the page to collect.

The entry point is a button on the app's Status tab (`openWifiDiag()` in `main.js`), which opens the
page in a new tab and falls back to in-place navigation if a popup blocker returns null.

## Tests

`tools/webtest/wifi_diag.test.mjs` evaluates the page's inline script in a `vm` sandbox. Beyond the
formatters, it pins two **cross-file** invariants that no reviewer would catch by eye across a `.c`
and an `.html`:

- the page's `rssiClass()` colour boundaries line up with `wd_hist_bucket()`'s histogram edges, so
  the page cannot paint a reading green while the report from the same device calls it weak;
- the low-memory threshold is the same number in both places, and is inside a sane range — a value
  above the internal RAM a device actually has free would make the finding permanent;
- no `wd_evt_push()` format string mentions an SSID or BSSID, which is what keeps the masked report
  honest.

## The event hooks capture; the sampler formats

The five `wifi_diag_note_*` hooks are called from `wifi_mgr`'s event handling, which runs on
**`sys_evt`** — the ESP-IDF system event task, whose stack this component does not own and cannot
size. They used to do their formatting right there. One `event_log_emit()` costs roughly 800 B on
the caller's stack (`evl_vemit()` alone puts `detail[112] + ts[24] + line[192]` on it, then calls
`vsnprintf`, `localtime_r`, `strftime` and `snprintf`), and at the IDF default of 2304 B that was
not enough: a device boot-looped every ~13 s until safe-mode rescue, on two different firmware
versions. `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` was raised to 4608 as a stopgap; the
measurement that followed put the deep path at 2548 B used, so 2304 had been 244 B short.

Raising the stack made the desk bigger without reducing the paperwork, so #111 moved the paperwork:

- A hook now fills a **`wd_fact_t`** — an enum, a few ints, one `strlcpy`'d SSID, plus
  `gettimeofday()` and the uptime **captured at the event** — and pushes it into a static 16-deep
  ring (`s_facts[]`, ~1.6 KB of `.bss`) under the module's existing lock. Nothing formats. No
  `snprintf`, no masking, no emit. Anything the *event's own moment* knows and the drain cannot
  travels in the fact — the connect stopwatch, and the RSSI on both `GOT_IP` and `DISCONNECTED`,
  read from the 1 Hz sampler's cached snapshot rather than from `esp_wifi_sta_get_ap_info()`
  (a driver call on the system event task is exactly the work this mechanism exists to move). An
  unknown RSSI stays 0 and the format side leaves the field out rather than printing a fake 0 dBm.
- **`wd_drain_facts()`** pops the facts and does all of it — `wd_evt_push()`, `wd_mask_ssid()`,
  `event_log_emit_at()` — on a stack that owns itself.

Four things about that are load-bearing:

- **The timestamp travels with the fact.** `event_log_emit_at()` (added by #111) stamps the line
  with the time the event happened rather than the time it was formatted, and `wd_evt_push()` takes
  an explicit `up_s` for the same reason. The visible cost is that `events.log` is no longer
  strictly append-ordered across subsystems — a Wi-Fi line can land after a line from elsewhere
  that happened later. Sort by timestamp, not by position.
- **The push never blocks and never retries.** A full ring overwrites its *oldest* fact and counts
  the loss (`fact_dropped` in the `/wifi_diag` JSON, plus one `EVL_WARN` line per minute). Making
  the event task wait on anything is the failure class this exists to remove.
- **Two drainers, no duplicates.** The sampler drains once per tick, and `/wifi_diag` and
  `/wifi_diag/report` each drain first — so the evidence still appears if the sampler task failed
  to start, which `wifi_diag_init()` has always promised. A drainer *claims* a fact by copying it
  out and advancing the tail inside the same critical section, so no fact is ever formatted twice.
- **The gate and the throttle are decided at capture.** Whether the attempt line is debug-gated,
  and whether a `DROP` beats the `WD_EVL_REPEAT_MS` throttle, depend on when the event happened —
  so the hook decides and the verdict rides in the fact.

The one accepted loss: a fact captured but not yet drained (up to ~1 s) disappears if the device
crashes in that window. Before, the line reached the event log's RAM ring synchronously — when it
did not panic the device outright. A one-second forensic gap beats a boot loop. The crash reporter
itself is unaffected; it emits after reboot from RTC data.

## The sampler also watches someone else's stack

Since #111 nothing in this file formats on `sys_evt` — but the capture hooks still *run* there, and
"just one quick emit here" is exactly how the boot loop happened the first time. So the check stays
in `wifi_diag`: the component that owns the temptation owns the alarm. It is not wifi_diag-specific
in what it watches — anything anyone hangs on `sys_evt` trips it. (The alternative, a general
`task_health` component, buys a new task and its stack to run one comparison per second against a
one-row table. The rule for later: the day a *second* task earns a floor check, build it then.)

`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` is 4608, doubled and rounded rather than measured, so two
things watch it (#112):

- **`GET /wake_probe` reports `sys_evt_stack_free`** — bytes still free at that task's worst moment,
  read via `xTaskGetHandle("sys_evt")` exactly like the neighbouring `sleep_task_stack_free`.
  Worst-case use is `4608 − N`. A `0` means the handle lookup failed, not "no headroom left".
- **`wd_check_sys_evt_stack()` warns once per boot** below `WD_SYS_EVT_STACK_WARN_MIN_FREE`
  (1024 B), as `EVL_WARN` in the event log. Not debug-gated — the bad case is never hidden. It is
  the last action of each sampler tick, which since #111 reads as one story: format what the event
  task captured, somewhere safe, then check that the unsafe place stayed cheap.

Two things about that are load-bearing:

- **`uxTaskGetStackHighWaterMark()` is a historic minimum**, not a live reading. That is why reading
  it a second later from another task is just as good as reading it at the deepest moment, and why
  the warning is latched: within one uptime the number can only shrink, so re-emitting would repeat
  itself every second forever.
- **The check runs on the sampler task, never in a `wifi_diag_note_*` hook.** Emitting from a hook
  would spend ~800 B on the very stack that just proved short — the warning could cause the overflow
  it warns about.

⚠️ **A boot-and-idle soak does not measure the real worst case.** `event_log_set_debug()` runs late
in `app_main`, so the *first* connection attempt after any boot is treated as gated-off. Only a
*later* reconnect walks the deep path. To get the honest number: set `debug=enabled`, then force a
real disconnect/reconnect burst before reading `/wake_probe`.

**The floor is 2048, and it is measured.** F — `sys_evt_stack_free` after a forced burst of 13
disconnects with debug on — is **2588 B free of 4608**. The same procedure gave **2060** before
#111, so moving the formatting off this stack returned **528 B**.

1024 was right before #111 and became decoration after it: with 2588 free, one re-added ~800 B emit
lands near 1788 — still far above 1024, so the tripwire could not fire on the regression it exists
to catch. The rule for the replacement is a round number in `(F − 800, F − 512]`: above the lower
bound so a single re-added emit trips it, at or below the upper so deep `sys_evt` paths the bench
burst never exercised cannot false-alarm. That window is `(1788, 2076]`, giving **2048**. Re-measure
and re-apply the rule if the stack size or the cost of an emit ever changes.

The stack stays at 4608. Measured worst-case use is now **2020 B** (4608 − 2588), so the IDF
default of 2304 would leave under 300 B free — below any sane floor. The reclaimable RAM is 1–2 KB
against ~32 KB free, and the failure mode of guessing short is the boot loop this whole effort
exists to bury.

## Gotchas

- `wifi_diag` is a **leaf**: it requires only base IDF plus `event_log`. `main` depends on
  components, never the reverse, so the firmware version and the embedded page bytes are *injected*
  (`wifi_diag_set_fw_version()`, `wifi_diag_set_page()`) rather than read.
- The version comes from `dev_status_get_running_app_info()`, **not** the file-static
  `firmware_version[]` in `main.c` — nothing ever assigns that array (it reaches `wc_mdns_init()` as
  an empty string), so a report built from it would carry a blank build.
- `wifi_mgr_set_attempted_ssid()` is the single choke point where `last_attempted_ssid` is set on a
  path about to call `esp_wifi_connect()`. Four sites used to open-code the same `snprintf`. Route
  any fifth one through it or the connect-time stopwatch will silently miss that path.
- `heap_caps_get_largest_free_block()` walks the heap free lists under the heap lock. At 1 Hz on a
  priority-2 task that is negligible — but it is why the sampler must never move onto a hot path.
- Everything here covers the **current uptime only**. The reboot-surviving record is in the event
  log: `GET /event_log`, look for `WIFI` lines.
- A `wifi_diag_note_*` hook may **capture facts and nothing else**. No `snprintf`, no masking, no
  `wd_evt_push()`, no `event_log_emit()`. They run on `sys_evt`; see the section above for what
  happened the last time they did more.
