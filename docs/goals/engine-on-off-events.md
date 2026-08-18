# Goal: ship the ENGINE_ON / ENGINE_OFF event lines (rebase, build, flash, prove on the bench)

**/goal condition (ready to paste):**

```
/goal All acceptance criteria AC1-AC10 in docs/goals/engine-on-off-events.md hold, each demonstrated in the transcript by its stated check, and no constraint C1-C7 is violated.
```

---

## 1. The goal

The ENGINE_ON / ENGINE_OFF feature already exists as finished, uncommitted code in the worktree
`.claude/worktrees/agent-a090ef56a2584ae94` (branch `feat/engine-on-event`, based on `8054bd0`).
The code itself is sound — the design review below found **no bug in the state machine**. What is
left is: commit it, **rebase it onto `wican-pro` (`7355da8`)** — which is what actually fixes the
"stuck in a loop" report, see section 3 — build it, flash the bench device, and prove on the bench
everything that can be proven there. The happy path (a real ENGINE_ON line) needs the car and is
in the manual checklist, outside the /goal condition.

What the feature does (all in the worktree diff, nothing more to write):

- `polllog_engine_edge()` (`components/fast_log/poll_log.c:611`) is called once per sweep from
  `polllog_eval_gate()` (`poll_log.c:685`), **above** the CSV-session shortcut (`poll_log.c:687`),
  and is fed the gate's own `rpm_known` / `rpm_running` verdict (`poll_log.c:675-684`).
- Rising edge: first sweep with a fresh rpm over `POLLLOG_GATE_RPM_ON` (400, `poll_log.c:178`)
  emits one `ENGINE_ON` line and latches `s_engine_on` (`poll_log.c:563-575`).
- Falling edge: rpm confirmed under the threshold and held there 3 s
  (`POLLLOG_ENGINE_OFF_CONFIRM_MS`, `poll_log.c:224`), or the ECU going silent (the quiesce at
  `poll_log.c:1571` calls `polllog_engine_stopped("ECU stopped answering")`). One `ENGINE_OFF`
  line with the run duration; the latch clears.
- New `poll_log_engine_running()` (`poll_log.c:1843`) exposed as `"engine_running"` in the
  `GET /poll_status` JSON (`poll_log.c:1902,1927`) — the only live view of the latch, since the
  bench can never make it true.
- Event codes registered in the enum (`components/event_log/include/event_log.h:69-70`) and the
  name table (`components/event_log/event_log.c:105-106`).

## 2. Design facts, verified against the code

These were all checked line by line; the implementer does not need to re-derive them.

**Registration is complete.** The enum and the `evl_code_str` name table both carry the two new
codes. A grep across the whole tree shows `evl_code_str` is the **only** switch over
`event_log_code_t` — there is no severity table and no web-UI filter list to update (the events
page streams plain text; no `*.html` / `*.js` file anywhere references event-code names). The
on-disk line stores the code as its **name string** (`event_log.c:172-173`), never as a number,
so inserting the codes mid-enum cannot mis-label old logs. The name table has no `default:`, so
`-Wswitch` under `-Werror` makes the build itself the registration check (AC3).

**Detail lengths are safe.** `EVENT_LOG_DETAIL_MAX` is 112 (`event_log.c:51`) and truncation is
silent. The claimed worst cases check out exactly: `"engine started -- 99999 rpm, volts n/a"` is
38 chars; `"engine stopped after 1193046h28m -- ECU stopped answering, volts n/a"` is 68. The
true buffer-bounded ceiling (dur[16], why[16], volts[12] — every feeder is clamped or truncated
by its own snprintf) is 70 chars. Comfortable margin; nothing to change.

**No task race.** Both writers of the latch run on the **same task**: `polllog_eval_gate` is
called from `polllog_rx_task` at `poll_log.c:1334`, and the quiesce stop path is inside the same
task's loop at `poll_log.c:1571`. The httpd task only *reads* the `volatile bool s_engine_on`
(`poll_log.c:390`, via `poll_log_engine_running`). The two plain `int64_t` timestamps are
touched only by the poll task. Correct as written.

**ENGINE_ON lands before DATALOG_OPEN — guaranteed, with one honest exception.**
Mechanism, in order:
1. On the poll task, the edge emit (`poll_log.c:685` → `event_log_emit` at `:574`) happens
   **before** the gate-open stores at `poll_log.c:691` and `:713` — plain program order in one
   function on one task. The event ring assigns sequence numbers under a critical section
   (`event_log.c:176-181`), so the ENGINE_ON line's sequence is fixed at that instant.
2. The CSV logger only learns the gate opened through `poll_log_gate_open` (registered at
   `poll_log.c:1749`, consumed at `csv_logger.c:707`), which it polls on its own task at a
   ≤250 ms tick (`csv_logger.c:648`). It emits `EVL_DATALOG_OPEN` at `csv_logger.c:831` only
   after opening the file. So DATALOG_OPEN is always sequenced after ENGINE_ON — typically
   0-250 ms later plus file-open time.
3. In the real car the rpm edge also *precedes* the voltage half of the gate: ENGINE_ON needs
   only rpm > 400 (immediate at start), while the gate additionally needs volts ≥ `engine_volt`
   from an ADC that refreshes every 3 s (`poll_log.c:169-170`). So the line lands first.
   **The exception:** if the rpm channel is dead or stale for the whole drive (`rpm_known`
   false), trips still open on voltage alone and **no ENGINE_ON is ever written** — deliberate
   (`poll_log.c:213-217`): the feature refuses to claim an engine it cannot see. A manual CSV
   Start on the bench likewise produces DATALOG_OPEN with no ENGINE_ON. Both are correct.

**Flood safety.** Both emitters return immediately when the latch already has the right state
(`poll_log.c:565`, `:582`), so the up-to-100-Hz sweep can never repeat a line. The worst
*legitimate* cadence is one ON/OFF pair per 3 s stop-debounce — and reaching even that requires
the rpm value to genuinely cross 400 both ways, repeatedly. A pathologically broken rpm
expression oscillating across 400 could sustain ~2 lines/3 s (~2400/h) onto SD — ugly but
bounded, and the same broken channel would already be flapping the recording gate itself
(pre-existing, documented at `docs/internals/poll_log.md` under `rpm`).

**#111 needs no anticipation.** The uncommitted `docs/goals/111-event-log-off-event-task.md`
explicitly keeps `event_log_emit()`'s contract for every caller outside wifi_diag (§2.1 and
constraint C5 there). The poll task owns its 8 KB stack and already emits IGNITION lines from
these exact spots (`poll_log.c:947`, `:1580`) — nothing to change now or later.

## 3. The "stuck in a loop" bug — root cause, evidence, fix

**Root cause: the loop was the device boot-looping on the pre-existing #113 stack overflow,
not the engine state machine.** The feature branch is based on `8054bd0`, which still has
`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304`. With `debug=enabled`, one WiFi connection
attempt overflows the system event task's stack (`wifi_diag` formats an event-log line from
inside the WiFi event handler) and the device panics ~13 s after every boot, forever. The bench
device had `debug` **deliberately enabled** in exactly that time window (it was soaking the
stack fix). Flashing a build of this branch onto it reproduces a device "stuck in a loop" with
the engine off — which is precisely what was reported.

The evidence, in order of strength:

1. **The identical panic was reproduced on plain v1.19.0**, which contains zero lines of this
   feature — decoded backtrace `panic_abort ← vApplicationStackOverflowHook ←
   vTaskSwitchContext` on the sys_evt task. That alone proves the loop is not this code.
2. **The state machine cannot emit anything at rpm 0.** ENGINE_ON strictly requires a fresh
   `s_rpm_value > 400` (`poll_log.c:613`, `:682`), and the bench PCM reports rpm 0 on **both**
   feeds — the polled PID and the 0x201 broadcast (bench-documented: broadcast filler values
   are RPM 0). ENGINE_OFF is a no-op unless latched (`poll_log.c:582-583`). Zero grep hits for
   `engine started` / `engine stopped` in the surviving logs is exactly what "never emitted"
   predicts, and the opposite of what "emitted in a loop" predicts.
3. **Timeline.** Worktree created 20:50 on 2026-08-17; the boot-loop was investigated,
   root-caused and fixed (`0e05ffd` → merged as `7355da8`, #113) between 21:00 and 22:42 the
   same evening.

Every in-code loop mechanism was also examined and ruled out with lines:

| Candidate | Verdict |
|---|---|
| Latch cleared by the quiesce path, then instantly re-armed | Re-arming needs a fresh rpm > 400 (`poll_log.c:613`); impossible at rpm 0. Both paths are on one task, so no clear/re-set race exists. |
| `s_rpm_seen` behaviour with no RPM row / all timeouts | Initialised `false` (`poll_log.c:382`); set **only** by a real decoded value whose channel name is exactly "RPM" (`poll_log.c:509-516`). Never set ⇒ `rpm_known` stays false ⇒ the edge function does nothing at all — both the start (`:613`) and the stop-arm (`:621`) require `rpm_known`. |
| Stale or garbage value vs the 400 threshold | Staleness is bounded by `POLLLOG_RPM_STALE_MS` (`poll_log.c:679`); a garbage-high decode would be *constant*, producing one spurious ENGINE_ON, not a loop. Bench rpm is 0 on both feeds. |
| Race between the two latch writers | Same task (`poll_log.c:1334` and `:1571`); httpd only reads. No race. |
| ECU flapping silent/answering (quiesce cycling) | Produces IGNITION_ON/OFF pairs (pre-existing, bench-proven when the PCM bounces), but ENGINE lines still need rpm > 400 to latch first. Cannot drive the pair. |
| Unlatched emit at FAST cadence | Latch is the first check in both emitters (`:565`, `:582`). Cannot repeat. |

**The fix is the rebase.** Rebasing `feat/engine-on-event` onto `7355da8` brings in the
sdkconfig stack raise (2304 → 4608) and the #110 UART-lock fix. The rebase is conflict-free by
construction: `git diff --stat 8054bd0..7355da8` touches only `components/wifi_diag/wifi_diag.c`,
`main/elm327.c`, `main/main.c`, `main/sleep_mode.c`, `sdkconfig` and a release-notes doc — zero
overlap with the feature's five files (`poll_log.c/.h`, `event_log.c/.h`,
`docs/internals/poll_log.md`). No change to the feature code itself is required.

🔴 **Hazard, stated plainly: do not flash this branch un-rebased.** The bench device may still
have `debug=enabled`; an un-rebased build boot-loops it ~13 s after boot, and recovery needs the
safe-mode rescue recipe. The rebase is not housekeeping — it is the bug fix.

## 4. How to verify on a bench where rpm is always 0

The bench can prove every **negative** guarantee and the plumbing; it cannot reach the happy
path (the gate comment at `poll_log.c:653` says so: a bench PCM reports rpm 0, so manual CSV
Start is the only way to reach FAST there). What the bench proves:

- The build is healthy and the device does **not** loop (one BOOT line, stable uptime).
- `engine_running` is present in `/poll_status` and reads `false` while `rpm_known:true, rpm:0`.
- Hours of sweeps at rpm 0 emit **zero** ENGINE lines — the false-positive guarantee, which is
  the half of the feature a loop bug would live in.
- A manual CSV session opens/closes with DATALOG lines and still no ENGINE lines (the
  session-shortcut hoist leaks no edge).

What genuinely needs the car: the ENGINE_ON line itself, its position before DATALOG_OPEN, the
ENGINE_OFF duration, and the one-pair-per-drive behaviour. Manual checklist, section 8.

## 5. Acceptance criteria (each verifiable from the session transcript)

Device addressing: try `wican_dcb4d91511b9.local` first, fall back to the last known IP
(192.168.1.169 at last check; `/check_status` reports `sta_ip`). Flash via the `wican-ota` skill.

- **AC1 — committed and rebased:** the worktree changes are committed on `feat/engine-on-event`
  and the branch is rebased onto `wican-pro`. Check: `git log --oneline -3` on the branch shown
  in the transcript, with the feature commit(s) directly on top of `7355da8`.
- **AC2 — the boot-loop fix rode along:** check: `grep CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE
  sdkconfig` on the branch shows `4608`.
- **AC3 — build:** `idf.py build` exits 0 (this is also the `-Wswitch` name-table proof).
  Check: the build's final "Project build complete" output in the transcript. Use the
  `wican-build` skill; remember the output `.bin` filename embeds `git describe` — flash the
  newest-named bin.
- **AC4 — name table:** check: grep output showing `EVL_ENGINE_ON` and `EVL_ENGINE_OFF` cases
  inside `evl_code_str` in `components/event_log/event_log.c`.
- **AC5 — edge above the shortcut:** check: Read/grep output showing `polllog_engine_edge(` on
  a line above the `csv_logger_session_active()` shortcut inside `polllog_eval_gate`.
- **AC6 — flash took:** check: `GET /check_status` output showing `git_version` equal to the
  new build's describe string (loop until it actually flips — one early success can be the old
  firmware still running).
- **AC7 — status field:** check: `GET /poll_status` output containing `"engine_running":false`
  alongside `"rpm_known":true` and `"rpm":0`, and the body parsing as JSON (no truncation).
- **AC8 — no false positives:** after a soak of at least 10 minutes with the PCM answering
  (`ok` counter visibly rising between two `/poll_status` samples), check: `GET /event_log`
  (and `/event_log/ram`) output shown, and a grep of it for `ENGINE_ON|ENGINE_OFF` showing
  **zero** matches.
- **AC9 — no loop:** check: the `/event_log` tail from AC8 contains exactly **one** `BOOT` line
  after the OTA, and `/check_status` shows the unexpected-reset counter unchanged from its
  pre-flash value (record it before flashing).
- **AC10 — manual session leaks no edge:** `POST /csv_logger?op=start`, wait ~10 s,
  `POST /csv_logger?op=stop`. Check: `/event_log` output showing the new `DATALOG_OPEN` and
  `DATALOG_CLOSE` lines and still zero ENGINE lines; `/poll_status` during the session shows
  `"gate_open":true` with `"engine_running":false`.

## 6. Constraints — what must NOT change

- **C1 — gate behaviour.** The recording gate opens and closes exactly as before; the hoisted
  rpm block (`poll_log.c:667-685`) stays pure reads. No second engine detector.
- **C2 — no new task, no new timer.** The diff adds none; keep it that way.
- **C3 — event-log on-disk line format** (`"%s up=%lldms %-12s %s"`, `event_log.c:172`) and all
  existing HTTP routes/fields unchanged. The one additive field is `engine_running` in
  `/poll_status`.
- **C4 — no unlatched emits.** Any edit to the emitters must keep the latch check first; an
  unlatched line at FAST cadence is up to 100 lines/s onto the SD card of a device whose only
  diagnostic channel is this log.
- **C5 — feature code frozen except for rebase mechanics.** The state machine was reviewed and
  cleared; do not "improve" thresholds, debounces, or message text on the way through.
- **C6 — sdkconfig:** `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4608` must survive the rebase
  (AC2 exists because losing it re-creates the boot loop).
- **C7 — single-vehicle product:** no per-car configuration, no vehicle profiles.

## 7. Scope discipline for the lunchtime flash

Required: AC1-AC10, nothing else. Explicitly deferred (do not do now): #111/#112 work, any rpm
channel-quality hardening, event-page UI work, merging/releasing (bench-test-before-merge rule:
the branch merges only after the road test), and the v1.19.1 re-release (separate open item).

## 8. Manual checklist — OUTSIDE the /goal condition

Before the drive (owner or supervised bench work):

- [ ] 🔴 Set `debug=disabled` before the road test (it writes an SD line per WiFi event, and
      the 4608 stack is a mitigation, not a measured fix — #112 is still open).
- [ ] 🔴 Restore the real sleep config: `sleep_time` is still the bench test value `1` (should
      be `5`); `sleep_volt` is already 13.0. Verify via `/check_status` after posting.
- [ ] Optional bench extra (needs hands — the WiCAN and PCM share one 12 V rail, so unplug the
      PCM's **CAN pair**, not its power): ≥10 s unplugged then replug → one IGNITION_OFF /
      IGNITION_ON pair appears, still no ENGINE lines (proves the quiesce stop path no-ops with
      the latch down).

The road test (the feature's actual proof):

- [ ] Drive a normal trip. Afterwards download `/event_log` and check, in this order:
- [ ] Exactly **one** `ENGINE_ON` line per engine start: `engine started -- ~800 rpm, ~14V`,
      positioned **before** that trip's `DATALOG_OPEN` line.
- [ ] Exactly **one** `ENGINE_OFF` at key-off with a plausible duration
      (`engine stopped after XmYs -- 0 rpm, ...` or `-- ECU stopped answering, ...`).
- [ ] No repeated ON/OFF pairs mid-drive. A mid-drive pair means a real stall or an rpm-channel
      dropout — report it with the log rather than dismissing it.
- [ ] `GET /poll_status` while the engine runs shows `"engine_running":true`.
