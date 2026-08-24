# The recording gate lies about the engine

`/goal` condition (ready to paste — also repeated at the bottom):

> /goal All acceptance criteria AC1–AC11 in docs/goals/recording-gate-lies-about-the-engine.md hold, each demonstrated in the session transcript, and every constraint C1–C10 is respected.

## The complaint, in one paragraph

On the bench (a real Mazda PCM at 13.9 V, no engine, RPM reads 0), the CSV datalogger started
recording **on its own** after the dead-man reaper auto-resumed a paused datalog session, and wrote
a **347 MB** CSV over 12 hours of an engine that was never running. The config plainly says
`csv_require_engine=enable` — the UI label reads "On (log only while engine running)". The morning
after, the device shows `gate_open:true` and `state:"fast"` with `rpm:0` and no session open — and
a second look proved those fields are a **frozen snapshot**: two `/poll_status` reads 10 s apart
show zero sweeps, zero OKs. The poll task is not running at all (Defect 0 below).

## Root cause — four defects

### Defect 0 — the poll task is WEDGED on an unbounded wait (found second, listed first because it colours every live reading)

Two `/poll_status` reads 10 s apart show `sweep_seq`, `ok`, and `gate_skips` all frozen, while
`state:"fast"`, `gate_open:true`, `quiesced:false`. Every field in `/poll_status` is a plain read
of the poll task's variables, so the whole JSON is a snapshot of the instant the task stopped —
~07:45 by the `/datalog` bus/diag idle clocks, which are stamped by the poller's own traffic
(main/can.c:717-725, :756-764). The PCM was answering the whole time (proven by the O-test below:
ok +1,359 immediately on resume), so the 20-minute silent bus was entirely the adapter having
stopped transmitting, not the car being off.

Where it is stuck: `can_receive()` (main/can.c:657-668) opens with
`xEventGroupWaitBits(CAN_ENABLE_BIT, …, portMAX_DELAY)` — an **unbounded** wait that ignores the
caller's `ticks_to_wait`. That wait is the intended OTA/sleep teardown fence (design comment,
poll_log.c:42-46): `can_disable()` → `can_block()` clears the bit (can.c:158), the consumer parks
inside the fence, and the later `can_enable()` → `can_unblock()` re-arms it via a timer
(can.c:512, :612-615). The fence is only safe if **every** clear is followed by a successful
enable. Two paths break that promise:

- `can_enable()` returns early on `twai_driver_install`/`twai_start` failure (can.c:486, :498,
  :509) **without** reaching `can_unblock()` at :512 — the bit stays clear forever.
- The SLCAN command **'C'** (close channel) calls `can_disable()` with no paired enable
  (main/slcan.c:298-306). Port **35001 is always on in every protocol** and its bytes are routed
  through `slcan_parse_str()` (main/main.c:279-291, after the fastread/fastwrite command sniff) —
  so **any TCP client that sends a byte 'C' to port 35001 uninstalls the CAN driver and hangs the
  datalogger permanently, in any mode, with nothing written to the event log** ('C' logs only to
  the unreachable serial console).

The frozen counters identify the moment precisely and rule the task's own quiesce flip out:
`timeout:7` and `txfail:0` in total. Had the task lived through key-off, 5 s of a silent bus at
full rate produces ~150+ timeouts before the quiesce flip at poll_log.c:1550-1558 even runs
(15 PIDs × 30 ms waits per sweep). Seven timeouts means the task froze within ~one sweep of its
last successful exchange — and `polllog_poll_one()` **starts** with a `can_receive` drain
(poll_log.c:880) *before* any counter can tick, which is exactly the profile of an external
`can_disable()` landing mid-sweep. The device is reachable over WiFi (rules out sleep, which
deinits WiFi, sleep_mode.c:543-547), no OTA ran, no park/claim was armed. The only trigger left
standing is an SLCAN 'C' on 35001 — the owner's PC tooling was live that morning (DATALOG_RESUME
07:29:36), and the frozen `fast`/`gate_open` combination points to a 'C'→'O'→'C' cycle at ~07:45
(see Defect 3's window arithmetic). Unproven but
falsifiable: sending `O\r` to port 35001 runs `can_enable()` (slcan.c:273-283), re-arms the fence,
and the wedged task must resume within ~1 s — that is both the confirmation and the no-reboot
recovery (manual checklist item 0).

**Bench-confirmed 2026-08-24:** sending `O\r` to 35001 resumed the task instantly (sweep_seq
+144 in ~3 s after 20 minutes of zero). Two refinements from that test: the gate closed by
itself within one pass (`gate_open` true→false, state fast→watch) — the close path is healthy
and "stuck open" was purely the frozen variable; and the ECU had been answering the whole
wedge (ok +1,359 with one timeout on resume), so the 20-minute silent bus was entirely the
adapter not transmitting, which makes the counter-fingerprint argument above conclusive.

Two field consequences worse than anything on the bench:

- **The wedge freezes `s_ecu_answering` at true, and the sleep veto reads it**
  (`poll_log_ecu_answering()`, poll_log.c:1830-1833, consumed at sleep_mode.c:1892). A wedged
  task means the veto holds **forever**: in a car, after key-off the device would never sleep
  → **flat car battery in days**. The veto's own design notes (poll_log.c:1816-1824) anticipate
  frozen-true only for the parked case, which `!can_should_park()` covers; the wedge is a
  frozen-true with no park, which nothing covers.
- Datalogging is silently dead until reboot — a real drive would be **lost without a trace**,
  and if a CSV trip had been open at the wedge, the writer's grid timer would keep emitting
  frozen-value LOCF rows forever (csv_logger.c:746-749 needs no records, only an open session).

### Defects 1-3 — how the 347 MB file happened (the poll task was alive and sweeping for all of it)

All line numbers are branch `wican-pro` at b325857.

**Defect 1 — the gate treats "RPM unknown" as "engine running", and a park freezes RPM into
"unknown".** The recording gate `s_gate_open` is a voltage+RPM state machine
(`polllog_eval_gate()`, `components/fast_log/poll_log.c:667`). Its RPM term only counts while the
last RPM sample is younger than `POLLLOG_RPM_STALE_MS` = 2000 ms (poll_log.c:180, :680-688); older
than that, the term drops out and the gate becomes voltage-only — a deliberate fail-open so a dead
RPM channel can never lose a real drive (design note poll_log.c:663-666). But while the poll task
is **parked** for a host session (`can_should_park()` short-circuits the whole loop,
poll_log.c:1277-1281), no sweeps run, so no RPM samples land (`polllog_stamp_rpm`, poll_log.c:509,
fed only from the sweep decode at :931 and the broadcast decode at :810). After the 78-second park
(19:03:05 → 19:04:23), the first gate evaluation saw a 78-s-old RPM sample → "unknown" → voltage
only → 13.9 V ≥ 13.2 V → **gate OPEN** (poll_log.c:713-722; `s_ecu_answering` was frozen `true`
across the park, so the open condition held). The reaper had just restored manual mode to AUTO
(`datalog_restore_mode()`, `components/csv_logger/csv_logger.c:1291`; pre-pause mode snapshotted at
:1358 was AUTO), the writer saw `ignition_on && engine_ok` both true (`csv_logging_active`,
`components/csv_logger/csv_bringup_logic.c:79`), records were already flowing from the first sweep,
and the session opened at 19:04:23.

**Defect 2 — the session then keeps the gate open, and the gate keeps the session legal
(circular latch).** One sweep later RPM was fresh again and read 0, which should have closed the
gate 3 s later — but `polllog_eval_gate()` returns early whenever a CSV session is open, forcing
`s_gate_open = true` and resetting the off-debounce (poll_log.c:691-700). Meanwhile the CSV
writer's `engine_ok` **is** `poll_log_gate_open()` = `s_gate_open` (registration poll_log.c:1755;
consumer csv_logger.c:707). So: session ⇒ gate open ⇒ `engine_ok` ⇒ session stays legal. Once a
session opens *for any reason*, nothing about the engine or the voltage can ever end it; only a
manual Stop, SD removal, voltage ignition-off, or sleep can. That is the 12 hours and 347 MB.

**Defect 3 — parks manufacture RPM staleness, and a stale-but-seen RPM opens the gate on voltage
alone.** Two status lies made this hard to see: `/poll_status` prints `rpm_known` as `s_rpm_seen`
(poll_log.c:1931) — "an RPM sample was seen at least once this uptime" — while the gate uses the
2-second freshness check (`POLLLOG_RPM_STALE_MS`, poll_log.c:180, :680-688); and the whole JSON
can be a frozen snapshot (Defect 0). The mechanism needs **no misconfiguration at all**:

- **The vehicle's RPM row is ungated** — settled on the bench 2026-08-24 by a correct read of the
  loaded table: RPM (`010C1`) carries no `SampleEvery`, so it is polled on every sweep (fresh
  every ~38 ms in FAST, every ~1 s in watch). The 7 gated rows are KNOCKR(2), HIDET_SW(8),
  STFT(8), LTFT(8), VCT_ACT(4), FUELSYS(8), BARO(16) — matching `pids_gated:7`; `std_pids` is
  empty; `reload_pending` was false. An earlier "no SampleEvery anywhere" read was a fetch error.
  A divisor-starvation theory for the gate was therefore **wrong** and is withdrawn.
- With RPM fresh, the gate behaves: any voltage-only opening closes 3 s after the first fresh
  RPM=0 sample (the O-test showed exactly this — gate open→closed, fast→watch, ~3 s after the
  un-wedge). So a `gate_open:true` reading can only be (a) a ≤3 s transient, or (b) a frozen
  snapshot, or (c) sustained by the session shortcut (Defect 2).
- What creates the transients: **any pause of the poll task longer than 2 s** — a host park
  (78 s on 23-Aug), a wedge (Defect 0), or a claim — leaves `s_rpm_seen=true` with a stale
  stamp. On the first pass after the pause the gate sees RPM as "unknown", falls back to
  voltage-only (the deliberate fail-open, poll_log.c:663-666), and **opens at bench voltage**
  before the very next sweep can deliver the fresh 0 that would have kept it shut.
- The final frozen state (`fast`/`gate_open:true` with bus activity until ~07:45) is consistent
  only with the wedge landing **inside** one of those ≤3 s reopen windows. A 35001 client's own
  SLCAN traffic manufactures exactly that: 'C' pauses the poller (wedge, RPM going stale),
  'O' resumes it (gate opens voltage-only on the stale pass), a final 'C' within ~3 s freezes it
  mid-window. Between 07:20:37 and that last cycle the gate sat correctly closed in watch. The
  exact byte sequence is unproven; the window arithmetic is not.

The design's stated fail-open ("RPM stale ⇒ voltage decides") plus a bench supply above
`engine_volt` therefore opens the gate after **every** pause — and Defect 2 then decides whether
it is a 3-second blip (no session) or a 12-hour runaway (session latches it). The 29-second boot
window (question B) behaved **correctly**: RPM stamped a fresh 0 from the first sweeps, the gate
stayed shut, so no session; the park then manufactured the staleness that opened it.

## The design

Six changes — five in `components/fast_log/` plus a status/doc pass, and one (R6) that touches
`main/can.c`/`poll_log.c` to remove the wedge class. The CSV side
(`csv_bringup_logic.c` truth table, `csv_logger.c` writer loop) is **not** touched — the switch
`csv_require_engine` is wired to the right *kind* of signal; the signal itself lies. Do NOT rewire
it to `poll_log_engine_running()` (the `s_engine_on` latch, poll_log.c:1847): that latch can only
become true after a real >400-rpm sample, so on any config where the RPM channel is missing or dies
it would silently record **nothing for an entire drive** — losing a real drive is worse than
logging a bench idle, so that option is rejected.

### R1 — the session may force the sweep RATE, never the engine ANSWER

Split the two meanings of `s_gate_open`:

- `s_gate_open` becomes the pure engine gate: the volt+RPM state machine in
  `polllog_eval_gate()` runs on **every** pass — delete the early-return CSV-session shortcut at
  poll_log.c:691-700 (the engine-edge hoist comment at :674-678 becomes moot; keep the edge call).
- The *sweep-shape* decision gains the session term instead. In `polllog_rx_task()` compute once
  per pass, right after `polllog_eval_gate(now)` (poll_log.c:1339):
  `const bool fast_sweep = s_gate_open || csv_logger_session_active();`
  and use `fast_sweep` where the loop currently reads `s_gate_open` for **rate/shape** purposes:
  `watch_mode` (:1342), `gate_active` for the divisors (:1361), the fast-rate stats condition
  (:1510), and any pacing/watch-delay site below that reads `s_gate_open` — audit every read of
  `s_gate_open` in the task loop and classify each as "engine question" (keep) or "rate question"
  (switch to `fast_sweep`). The quiesce close at :1564 keeps writing `s_gate_open = false`.
- `poll_log_gate_open()` (:1837) is untouched in shape and now returns the honest engine gate, so
  the CSV `engine_ok` (csv_logger.c:707) becomes honest with **zero** CSV-side change.

Behavioural consequences, all intended: a manual (FORCE_ON) bench session still sweeps at full
rate (session term) and still records (FORCE_ON beats `engine_ok` in the truth table,
csv_bringup_logic.c:82-85). An AUTO session now **ends ~3–6 s after the engine stops** (gate
closes → `engine_ok` false → writer closes with the existing `"engine_off"` reason,
csv_logger.c:729-735) — one file per engine stint, which is what the switch's label promises.

### R2 — the gate's own sensor is never divisor-starved (HARDENING, not load-bearing)

**Status: hardening only.** The bench settled that this vehicle's RPM row is ungated, so no
observed defect involved a divisor. Keep R2 anyway because the failure it prevents is silent and
data-shaped: a future config that puts `SampleEvery >= 53` on RPM (53 × 38.4 ms > the 2 s
freshness window) would make the gate flip-flop known/unknown and never complete its 3 s
off-debounce. One small change, cheap insurance — but if it conflicts with anything, it is the
piece to drop, not R1 or R3.

In `polllog_prepare_schedule()` (poll_log.c:990), when a row has any enabled parameter whose name
is exactly `"RPM"` (case-insensitive — the same funnel `polllog_stamp_rpm` matches on,
poll_log.c:509-511), force that row's `sample_every` to 1 and `ESP_LOGI` a one-line note ("RPM
feeds the recording gate; SampleEvery overridden to every sweep"). Cost: a user divisor on RPM is
ignored; RPM costs ~2.4 ms per sweep, negligible.

### R3 — a stale-but-seen RPM does not open the gate; it waits for a fresh sample (THE core remedy)

**Status: load-bearing.** With the divisor theory withdrawn, park-induced staleness is the
*entire* observed opening mechanism (Defect 3), and R3 is the change that closes it: every
pause longer than 2 s — host park, claim, wedge — currently buys a voltage-only opening on the
first pass after resume, before the next sweep can deliver the fresh RPM=0. Because RPM is
polled every sweep, the fresh verdict R3 waits for arrives on the very next pass, so its cost
is ~40 ms in fast and ≤1 s in watch.

In the closed→open branch (poll_log.c:713-723) add a wait: if `s_rpm_seen` is true but the sample
is stale (`!rpm_known`), do **not** open yet; count evaluation passes in a small static and only
fall back to voltage-only opening after `POLLLOG_RPM_CONFIRM_SWEEPS` (3) consecutive blocked
passes. Reset the counter whenever `rpm_known` is true or the gate is open. Because probe and
watch sweeps poll every PID (divisor bypasses 1 and 3, poll_log.c:1347-1358) and R2 covers FAST,
the very next sweep normally delivers a fresh verdict, so the wait is one pass in practice.

- Reaper-resume on the bench: first pass blocked (stale), sweep stamps RPM=0, second pass sees
  fresh 0 → gate stays closed → **no session, not even a junk file**. (R1 alone would still allow
  a ~3–6 s junk file per resume; R1+R3 allow none. They are independent fixes — defense in depth.)
- Resume mid-drive on a real car: blocked ≤1 s, then fresh 3000 rpm opens it. Bounded loss ≤~1 s.
- Truly dead channel (cause (b)): blocked for 3 passes (~3 s in watch), then voltage-only opening
  — the shipped fail-open survives, merely 3 s later, so a dead sensor still cannot lose a drive.
- Never-configured channel (`s_rpm_seen` false): unchanged, voltage-only immediately.

### R4 — the status stops hiding the gate's real RPM verdict, and a frozen task becomes visible

In `poll_log_get_status_json()` (poll_log.c:1881): make `rpm_known` report the same
freshness-checked verdict the gate computes (age ≤ `POLLLOG_RPM_STALE_MS`), add `rpm_seen`
(the old `s_rpm_seen`) and `rpm_age_ms` (ms since last stamp; emit a large sentinel or omit when
never seen). Also add `loop_age_ms`: stamp a volatile `s_last_loop_us` once per pass at the top of
`polllog_rx_task`'s loop and emit the age — a wedged or parked task then shows up in one
`/poll_status` read instead of masquerading as live data (this investigation lost a full round to
exactly that). Check `POLLLOG_STATUS_JSON_SZ` (1024, :255) still fits — the function already screams
on truncation (:1936); bump the size if needed. Grep `main/web/src/main.js` for `rpm_known` and
keep the UI consistent (rebuild via `tools/build_web.py` + `tools/lint_web.py` if the UI is
touched — never hand-edit `src/homepage.html`).

### R5 — make the gate testable on the host, and pin it

The three rules above are exactly the kind of subtle that regresses. Extract the gate decision
into a pure function with no ESP-IDF includes — same pattern as
`components/csv_logger/csv_bringup_logic.c` (see its header for the rationale):
`components/fast_log/poll_gate_logic.c/.h`, holding the state struct (gate open, low_since,
confirm counter) and a step function taking (now_us, session_active, have_volts, volts,
rpm_seen, rpm_known, rpm_running, ecu_answering, gate_volt_on). `polllog_eval_gate()` becomes a
thin adapter that gathers inputs and applies outputs/log lines. Add
`tools/hosttest/poll_gate_test.c` to `tools/hosttest/run.sh` pinning at least:

1. Bench stuck-open (this bug): fresh RPM=0 at 13.9 V closes the gate after the debounce and it
   stays closed; a session must NOT reopen it.
2. Reaper-reopen (this bug): stale-but-seen RPM at 13.9 V does not open the gate; a fresh 0 on the
   next pass keeps it closed.
3. Dead channel: seen-then-stale RPM opens voltage-only after exactly 3 blocked passes.
4. No RPM channel ever: voltage-only, opens immediately (shipped behaviour preserved).
5. Mid-drive staleness: gate open + RPM goes stale ⇒ gate stays open on voltage (never lose a
   drive); RPM back fresh at 0 ⇒ closes after the 3 s debounce.
6. RPM flip-flop (known 2.0 s / unknown 0.5 s cycles) with the session term removed: the gate
   still closes once R2's freshness holds — i.e. the debounce is only resettable by a *positive*
   running verdict, never by mere staleness while the gate is open. (Encodes the R1 semantics.)

### R6 — the poll task must never block forever on the CAN fence (Defect 0)

The wedge class: `CAN_ENABLE_BIT` cleared with no matching successful `can_enable()` parks the
poll task inside `can_receive()`'s `portMAX_DELAY` wait (main/can.c:664-668) permanently, freezing
`s_ecu_answering` at true — which holds the sleep veto up forever (sleep_mode.c:1892) and can flat
a car battery. Three parts:

- **R6a — a non-blocking receive for the sole-consumer poll path.** Add `can_receive_nb()` in
  main/can.c: check `CAN_ENABLE_BIT` with `xEventGroupGetBits` (the same fast-fail shape
  `can_send()` already has, can.c:749-751); if clear, return `ESP_ERR_INVALID_STATE`; else
  `twai_receive(message, 0)` plus the same activity stamps as `can_receive`. Switch **every**
  `can_receive` call in poll_log.c to it (the drains at :880, :902, :1125, :1137, :1165, :1412,
  :1475, :1607 — all already pass 0 ticks). The blocking `can_receive()` keeps its exact
  semantics for its other callers (elm327/fast_log/can_rx_task use the infinite fence as their
  park — do not touch them).
- **R6b — a "bus withheld" park with fail-closed signals and a bounded self-heal.** At the top of
  `polllog_rx_task`'s loop, next to the `can_should_park()` check (poll_log.c:1277): if
  `!can_is_enabled()` and NOT parked/claimed/flashing, enter a withheld-park: short sleep
  (`POLLLOG_FLASH_PARK_MS` cadence), and **clear `s_ecu_answering`, `s_gate_open`** (nothing is
  maintaining them — the fail-closed direction the veto's own notes demand, poll_log.c:1810-1824;
  this drops the sleep veto and closes the CSV engine gate within one writer poll). After a
  bounded grace (`POLLLOG_BUS_WITHHELD_MS`, 10 s) still disabled and still ownerless, re-run the
  poller's own bring-up (`can_set_silent(0); can_enable();` — the same bracket the resume path
  uses, poll_log.c:1618-1620) and emit `EVL_WARN` "CAN bus was left disabled with no owner --
  datalogger re-enabled it". Never self-heal while `can_should_park()` is true (a real host
  session owns the bus), and never touch `FLASH_ACTIVE_BIT`.
- **R6c — context for the reviewer, not code:** the reachable trigger today is the SLCAN 'C'
  command (slcan.c:298-306) arriving on the always-on port 35001 (routed via main.c:279-291 in
  every protocol) — one stray byte from any TCP client kills the datalogger with no event-log
  trace. R6a+R6b turn that from a permanent silent wedge into a logged 10 s pause. Whether 35001
  should accept bare SLCAN channel commands outside a host session at all is a policy question —
  file it as a follow-up issue, do not change the 35001 routing in this goal (NC-Flash depends
  on it).

With R6 in place, R3's "wait up to 3 passes" always gets its passes (the task cannot silently
stop passing), and a wedge can no longer leave a stale `gate_open:true` for the CSV writer's
`engine_ok` to trust.

## Acceptance criteria

Each is one end state plus the check the implementer must demonstrate in its own transcript
output (the evaluator cannot run commands).

- **AC1 — firmware builds.** `idf.py build` exits 0 (use the `wican-build` skill environment);
  the transcript shows the successful build tail.
- **AC2 — host tests pass, including the new gate suite.** Transcript shows the output of
  `tools/hosttest/run.sh` with the existing `csv_bringup_test` still passing and the new
  `poll_gate_test` passing all cases listed in R5.
- **AC3 — the pure logic file is IDF-free.** Transcript shows
  `grep -n "esp_\|freertos" components/fast_log/poll_gate_logic.c` producing no include hits
  (comment mentions allowed).
- **AC4 — the session no longer writes the engine gate.** Transcript shows a grep of the final
  `polllog_eval_gate` / gate-step code demonstrating no `csv_logger_session_active()` term sets or
  sustains the engine-gate state, and a grep showing `fast_sweep` (or equivalent) ORs the session
  term at the sweep-rate consumers (former poll_log.c:1342/:1361 sites).
- **AC5 — RPM rows are never divisor-gated.** Transcript shows the `polllog_prepare_schedule`
  hunk (grep or diff excerpt) forcing `sample_every = 1` for rows with an enabled parameter named
  "RPM", matched case-insensitively.
- **AC6 — stale-RPM open-block exists with a 3-pass bound.** Demonstrated by the passing host
  test cases 2 and 3 in AC2, plus a grep showing `POLLLOG_RPM_CONFIRM_SWEEPS` (value 3) in the
  gate logic.
- **AC7 — status honesty.** Transcript shows the `poll_log_get_status_json` hunk emitting the
  freshness-checked `rpm_known` plus `rpm_seen`, `rpm_age_ms` and `loop_age_ms`, and shows the
  result of grepping `main/web/src/main.js` for `rpm_known` with either "no consumer" or the
  updated consumer (and, if the web UI changed, the `tools/build_web.py` + `tools/lint_web.py`
  runs).
- **AC8 — the protected files did not change.** Transcript shows `git diff --stat` (or
  `git status`) for the final change set, demonstrating **no** modifications to
  `components/csv_logger/csv_bringup_logic.c`, `tools/hosttest/csv_bringup_test.c`,
  `components/can/` lease/park/claim code, `main/datalog_lease_task.c`, and no edits to
  `main/web/src/homepage.html` by hand. `docs/internals/poll_log.md`'s gate section is updated to
  describe R1–R3 and R6 (grep shows the new wording).
- **AC9 — poll_log never blocks on the fence.** Transcript shows `can_receive_nb()` exists in
  main/can.c with the `xEventGroupGetBits` fast-fail (no `xEventGroupWaitBits`), and
  `grep -n "can_receive(" components/fast_log/poll_log.c` returns zero plain `can_receive` calls
  (only `can_receive_nb`). The blocking `can_receive()` in main/can.c is unchanged for its other
  callers (diff excerpt shows no edit to its wait).
- **AC10 — withheld-park with fail-closed signals and bounded self-heal.** Transcript shows the
  `polllog_rx_task` loop-top hunk: the `!can_is_enabled()` branch clears `s_ecu_answering` and
  `s_gate_open`, guards the self-heal on `!can_should_park()`, uses a named
  `POLLLOG_BUS_WITHHELD_MS` (10 s) bound, and emits the `EVL_WARN` re-enable event. Firmware
  builds with it (AC1 covers the build).
- **AC11 — the sleep veto cannot freeze true.** Transcript shows, by grep of the final code, that
  every path which stops the poll task's loop from passing (withheld-park included) either keeps
  running `can_should_park()`-style bounded sleeps or clears `s_ecu_answering` first — stated as
  a short written argument in the transcript walking the loop's blocking points (the evaluator
  reads the argument plus the grep evidence; there is no runtime check it can run).

## Constraints — what must NOT change

- **C1** — Nothing touches `FLASH_ACTIVE_BIT`, the coexistence lease/park/claim logic in
  `components/can/`, or the dead-man reaper (`main/datalog_lease_task.c`).
- **C2** — The CSV truth table `csv_logging_active()` (csv_bringup_logic.c:79-86) and its host
  test stay byte-identical. FORCE_ON must still record regardless of the engine gate; FORCE_OFF
  must still stop everything; the sleep request must still outrank FORCE_ON.
- **C3** — The sleep veto `poll_log_ecu_answering()` (poll_log.c:1830) and
  `poll_log_ignition_on()` (:1787) keep their exact semantics and fail directions — read the
  DO-NOT-MERGE banner in poll_log.h first.
- **C4** — Fail-open on voltage stays: an unreadable ADC (`sleep_mode_get_voltage` failing) still
  counts as `volt_ok` (poll_log.c:709).
- **C5** — *(amended during implementation, 2026-08-24, on Fable's ruling.)* A config with **no**
  RPM channel keeps today's voltage-only gate with no new delay. The no-delay promise keys on
  **CONFIGURED**, not on **SEEN**: the original wording let the implementation read "no sample yet
  this uptime" as "no channel", which is true for the first seconds of every boot and made the
  gate open on voltage and write a junk session on every boot. `s_rpm_configured` scans the PID
  table *and* the broadcast filters, each with the same validity rule its consumer applies. The
  original text follows.

  A config with **no** RPM channel keeps today's voltage-only gate with no new delay;
  the only added strictness anywhere is the ≤3-pass open-block for a channel that WAS seen and
  went stale, and the divisor override on RPM rows.
- **C6** — `polllog_engine_edge()` semantics (ENGINE_ON/ENGINE_OFF events, the load-bearing
  staleness asymmetry documented at poll_log.c:602-614) are unchanged.
- **C7** — Probe behaviour unchanged: `!s_ecu_answering` still sweeps full-rate with divisors
  bypassed, and opening the gate still requires `s_ecu_answering`.
- **C8** — No new writes to config.json from firmware (see the F2 hazard note in project memory),
  and no hand edits to generated web files.
- **C9** — The blocking `can_receive()` keeps its exact semantics (including the infinite fence
  wait) for elm327/fast_log/can_rx_task — they use it as their OTA/sleep park. Only poll_log
  moves to `can_receive_nb()`.
- **C10** — The 35001 port routing and the SLCAN command set are untouched (NC-Flash depends on
  them); the R6c policy question is a follow-up issue, not part of this change. R6b's self-heal
  must never run while `can_should_park()` is true.

## Manual bench checklist — explicitly OUTSIDE the /goal condition

Physical/on-device verification the owner (or the main session, with the bench device) does after
the change builds and the host tests pass:

0. **Unwedge and confirm Defect 0 (do this first, current firmware):** send the two bytes `O\r`
   to TCP port 35001 (e.g. `ncat <ip> 35001`), then read `/poll_status` twice, 10 s apart. If the
   diagnosis is right, `sweep_seq`/`ok` move again within ~1 s of the 'O' (that IS the recovery —
   no reboot needed; the poller will then run its normal quiesce against the keyed-off PCM). If
   the counters stay frozen, the wedge is somewhere else and Defect 0 needs re-investigation
   before R6 is built. A reboot also recovers either way.
1. **SETTLED 2026-08-24 — no device read needed.** The loaded PID table was read correctly:
   RPM is UNGATED (no `SampleEvery`); the 7 gated rows are KNOCKR(2), HIDET_SW(8), STFT(8),
   LTFT(8), VCT_ACT(4), FUELSYS(8), BARO(16); `std_pids` is empty; `reload_pending` false,
   `reload_ok` true. The divisor-starvation theory is refuted; the earlier table/status mismatch
   was a fetch error, not a device state. Kept here so nobody re-opens the question.
2. Flash the fix (OTA per the `wican-ota` skill), keep `csv_require_engine=enable`, AUTO mode,
   bench at ~13.9 V: within ~10 s `/poll_status` must settle to `state:"watch"`,
   `gate_open:false`, `rpm_known:true`, `rpm:0`, and `/datalog` must show no session for 10+
   minutes. No DATALOG_OPEN in the event log.
3. Manual Start from the web UI: session opens, sweeps go fast; Stop closes it. (FORCE_ON path.)
4. Replay the incident: `POST /datalog?op=pause`, then `op=bus_claim`, then kill the host and let
   the reaper fire. AUTO_RESUME must appear in the event log **without** a DATALOG_OPEN after it.
5. Real-car drive: trip records from shortly after engine start, closes with reason
   `engine_off` within ~10 s of switching the engine off; a restart opens a new file.
6. **Wedge replay (post-fix):** with no host session active, send `C\r` to port 35001. The
   datalogger must NOT hang: `/poll_status` `loop_age_ms` stays small, `ignition_on` drops to
   false (fail-closed `s_ecu_answering`), and within ~10 s the `EVL_WARN` "re-enabled" event
   appears and sweeping resumes. Then verify sleep is not vetoed in this state (the veto term
   must read false while the bus is withheld).
7. Delete the 347 MB CSV from the SD card, and restore the bench sleep-config values if still on
   test settings (see project memory).

## Follow-ups noticed, out of scope here (file as issues)

- No size or free-space cap on a CSV session: a runaway (or just a very long drive) can fill the
  SD card; on a full card the writer will churn close/retry cycles (csv_logger.c:751-757).
- After a manual Stop on the bench, FORCE_OFF only clears when the *voltage* ignition drops
  (csv_bringup_logic.c:70-77) — a PSU dip below threshold and back would have re-armed AUTO
  against the stuck gate. Fixed in effect by R1–R3, but worth remembering when reading old logs.
- Port 35001 accepts bare SLCAN channel commands ('O'/'C'/'L'/bitrate) from any client, in every
  protocol, with no session concept and no event-log trace (main.c:279-291 → slcan.c:271-318).
  Beyond the wedge (R6 bounds that), a stray 'O' can re-enable the bus in NORMAL mode at an
  arbitrary moment — policy follow-up per R6c.
- `can_enable()` is `void`: no caller can tell that install/start failed and the fence was left
  down (can.c:486/:498/:509). R6b makes the poll task survive it; the other callers still
  cannot — worth a return-code follow-up.

`/goal` condition (same as at the top):

> /goal All acceptance criteria AC1–AC11 in docs/goals/recording-gate-lies-about-the-engine.md hold, each demonstrated in the session transcript, and every constraint C1–C10 is respected.
