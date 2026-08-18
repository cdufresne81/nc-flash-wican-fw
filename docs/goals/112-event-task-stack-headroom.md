# Goal: measure the event task's stack headroom (issue #112)

**/goal condition (ready to paste):**

```
/goal All acceptance criteria AC1–AC6 in docs/goals/112-event-task-stack-headroom.md hold, each demonstrated in the transcript by its stated check. The manual checklist at the bottom is explicitly NOT part of this condition.
```

## The goal

`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` was raised 2304 → 4608 (commit `0e05ffd`,
already merged into `wican-pro` — `sdkconfig:1680` shows 4608) to stop a stack overflow
that boot-looped a real device until safe-mode rescue. 4608 was doubled-and-rounded, not
measured. This goal adds a permanent, cheap way to read that task's real worst-case stack
use over WiFi, plus a latched warning if it ever gets close to the edge — the same
treatment the sleep task already has.

After this ships, the claim "4608 fits" is backed by a number anyone can read with
`curl`, and any future change that eats into that headroom announces itself in the
event log instead of announcing itself as a boot loop.

## Facts the design stands on (verified in this codebase / this IDF)

- The task is the ESP-IDF default event loop task, named **`sys_evt`** — confirmed in
  the installed ESP-IDF v5.5.3 at
  `C:\esp\esp-idf-v5.5.3\components\esp_event\default_event_loop.c:100`
  (`.task_name = "sys_evt"`, stack = `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE`).
- The overflowing path: WiFi event handler `main/wifi_mgr.c:1375` →
  `wifi_mgr_update_last_attempted_from_current_config` (`main/wifi_mgr.c:210`, puts a
  whole `wifi_config_t` on the stack) → `wifi_mgr_set_attempted_ssid`
  (`main/wifi_mgr.c:202`) → `wifi_diag_note_attempt`
  (`components/wifi_diag/wifi_diag.c:436`) → the debug-gated emit at
  `wifi_diag.c:447`. One `event_log_emit()` costs roughly 800 B on that stack
  (buffers + newlib `vsnprintf`/`localtime_r`/`strftime`/`snprintf` — see the hazard
  comment at `wifi_diag.c:415-435` and the buffers in `evl_vemit`,
  `components/event_log/event_log.c:145-171`).
- The SD write is **already** off this task: `evl_vemit` only formats and pushes into a
  RAM ring; `evl_writer_task` (`event_log.c:326`, its own stack) does the file I/O. So
  what #111 will remove from `sys_evt` is the *formatting*, not the write.
- **The pattern to copy** exists twice:
  - Reading another task's headroom from an HTTP handler:
    `main/config_server.c:2357-2358` — `xTaskGetHandle("sleep_task")` +
    `uxTaskGetStackHighWaterMark(handle) * sizeof(StackType_t)`, reported as
    `sleep_task_stack_free` by `GET /wake_probe` (`config_server.c:2348-2376`).
  - The latched low-stack warning: `main/sleep_mode.c:1476-1490` —
    compare against a floor constant (`SLEEP_RESUME_STACK_WARN_MIN_FREE`,
    `sleep_mode.c:768`), emit **once per boot**, never per-event.
- Both FreeRTOS functions are already enabled and in use
  (`xTaskGetHandle` at `config_server.c:2357`, `uxTaskGetStackHighWaterMark` at
  `sleep_mode.c:1477`), so no sdkconfig change is needed.
- `wifi_diag` already owns a 1 Hz background task: `wd_sampler_task`
  (`components/wifi_diag/wifi_diag.c:694-702`, created at `:712`, priority 2).

## The one insight that shapes the design

`uxTaskGetStackHighWaterMark` returns a **historic minimum** — the closest the task has
*ever* come to the end of its stack, found by scanning the unused fill pattern. So:

1. **Where you read it does not matter.** Reading `sys_evt`'s mark from the HTTP task
   five seconds after a reconnect gives exactly the same worst-case number as reading
   it from inside the deep path. There is no need to sample at the deepest moment.
2. **Where you warn from matters a lot.** Emitting the warning from inside a
   `wifi_diag` hook would spend ~800 B on the exact stack whose headroom just proved
   low — the warning could cause the overflow it warns about. The warning must come
   from a task with its own stack.

This decides both open questions from the issue:

- **Reading route: `xTaskGetHandle("sys_evt")`, not in-hook `NULL` sampling.** The
  in-hook route's claimed advantage ("measures the right thing directly") buys nothing,
  because the mark is historic anyway — and it would put the check-and-warn on the
  endangered stack. The handle route mirrors the existing `sleep_task` line exactly.
- **Warning home: `wd_sampler_task`.** It already runs at 1 Hz in the same component
  that owns the hazard comment, on its own stack, at low priority. Zero new tasks,
  zero cost on `sys_evt`.

## Design

### 1. Surface the number on `GET /wake_probe`

In `wake_probe_handler` (`main/config_server.c:2348`), next to the existing
`sleep_task` lines, add:

```c
const TaskHandle_t evt_task = xTaskGetHandle("sys_evt");
const unsigned evt_hw = evt_task ? (unsigned)(uxTaskGetStackHighWaterMark(evt_task) * sizeof(StackType_t)) : 0u;
```

and a new JSON field `"sys_evt_stack_free":%u`. Grow `body` from 320 to **384** bytes —
the current worst case is already near 280 and a truncated response is corrupt JSON.

Why `/wake_probe` and not the others:
- `/check_status` (`config_server.c:1678`) is ~2 KB, rebuilds a dozen strings, and
  carries credentials — the wrong place for a diagnostic scalar.
- `/event_log/status` (`components/event_log/event_log.c:522`) belongs to a component
  that is deliberately a minimal leaf; the number is about the WiFi event task, not
  about the log.
- `/wake_probe` is the permanent no-console diagnostic endpoint, already reports
  `sleep_task_stack_free`, and its comment block (`config_server.c:2329-2347`) says
  exactly why it exists. This field is the same kind of thing. Extend its "what each
  field is for" comment with one line.

`xTaskGetHandle` walks the task list, but this endpoint already pays that cost per
request for `sleep_task` and is polled by hand, not on a hot path. `sizeof(StackType_t)`
is 1 on this port, so the multiply is a no-op kept for portability — same as the
existing line.

### 2. Latched low-headroom warning, checked from the sampler task

In `components/wifi_diag/wifi_diag.c`:

- Near the hazard comment (`:415-435`), add:
  ```c
  #define WD_SYS_EVT_STACK_WARN_MIN_FREE 1024
  ```
- Two new statics: a cached `TaskHandle_t s_sys_evt` (looked up once — the task never
  dies, so the handle stays valid) and a `bool s_sys_evt_stack_warned` latch.
- At the end of `wd_sample_once()` (`:600`), outside the critical section: if the
  handle is still NULL, try `xTaskGetHandle("sys_evt")` once per tick (it can fail in
  the first ticks before the default loop exists; that is fine, try again next
  second). Once held, read the high-water mark; if below the floor and not yet
  warned, set the latch and `event_log_emit(EVL_WARN, "sys_evt stack low: %u B free
  (floor %u B)", ...)` — mirroring the wording at `sleep_mode.c:1488`.
- Latched once per boot for the same reason documented at `sleep_mode.c:1479-1484`:
  the mark is a historic minimum, so within one uptime the condition can never clear;
  re-emitting would repeat the same number forever.
- The warning is **not** debug-gated — same rule as the sleep floor check
  (`sleep_mode.c:1472-1475`): the bad case must never be hidden.
- If the sampler task failed to start (`wifi_diag.c:712-717`), the warning is lost but
  `/wake_probe` still works — the two paths are deliberately independent.

1 Hz sampling loses nothing: the deep reconnect moment is captured by the stack fill
pattern and read later. `uxTaskGetStackHighWaterMark` on another running task is safe —
it scans the unused end of the stack, and the value only ever shrinks.

### 3. The floor: 1024 B, and why

- The most likely future growth is one more `event_log_emit` on this stack — ~800 B
  (measured in the incident, documented at `wifi_diag.c:419-421`).
- Interrupt entry also saves context on the running task's stack before switching to
  the interrupt stack — a couple hundred bytes must always stay free on top of the
  deepest call path.
- So: below 1024 B free, one added log line plus an ill-timed interrupt is a panic.
  1024 = "the next emit no longer fits". Above it, there is room to act before the
  cliff.
- The sleep task uses 2048 (`sleep_mode.c:768`) because its resume path runs the whole
  WiFi bring-up and is expected to grow; `sys_evt`'s job is supposed to *shrink*
  (#111), so the tighter floor is honest, not optimistic.

### 4. The measurement procedure (for the owner, on the bench device)

The number that closes #112 is `sys_evt_stack_free` read **after** the deep path has
actually run. The trap, which cost real time once already: **a boot-and-idle soak does
NOT exercise the crashing path.** `event_log_set_debug()` runs late in `app_main`
(`main/main.c:1179`), so the FIRST connection attempt after any boot is treated as
gated-off — only a *later* reconnect walks the full path down to the emit at
`wifi_diag.c:447`. Therefore:

1. Flash the instrumented build (branch already contains the 4608 stack — commit
   `0e05ffd` is in `wican-pro` — so enabling debug is safe; on older firmware it
   boot-loops the device).
2. Set `debug=enabled`, reboot, wait for the device to connect.
3. Force at least one disconnect/reconnect: take the AP down for ~10 s and bring it
   back (or kick the client from the router admin). A human or the router has to do
   this — the device cannot be asked to drop its own link over HTTP.
4. `curl http://<device>/wake_probe` and record `sys_evt_stack_free`.
5. Post the number to issue #112. Headroom = that number; worst-case use =
   4608 − that number.

### 5. Keep 4608 or adjust — the decision rule

- **Measured free ≥ 1024:** keep 4608. Do not shave it to reclaim RAM: the whole
  incident cost 2304 B of internal RAM to fix, the pool had ~32.8 KB free at the time,
  and the failure mode of guessing wrong is a device that bricks until safe mode.
  Saving a few hundred bytes is not worth re-opening that risk.
- **Measured free < 1024:** raise in 512 B steps until the measured free clears the
  floor with margin, each step justified by a new measurement — never by guesswork.
  (The warning latch will also have fired, which is the point of it.)
- **Only after #111 lands** does *lowering* become discussable: re-run the same
  measurement on the post-#111 build, and if the deep path no longer formats anything,
  the measured number itself justifies returning toward the IDF default 2304. Any
  lowering must cite a post-#111 measurement taken with the same debug-on + reconnect
  procedure. Until then, 4608 is the floor of what is known to work.

### 6. Interaction with #111

If #111 (move formatting off the event task) lands first, this work does not change
shape — it changes role:

- The instrumentation is identical; nothing here touches the code #111 rewrites
  except by *reading* a task's stack mark.
- The urgency drops (the emits stop riding `sys_evt`), but the monitor becomes the
  **proof that #111 worked**: `sys_evt_stack_free` should jump visibly, and that
  before/after pair is the evidence for shrinking the stack back.
- The latched warning becomes the permanent regression guard against anyone ever
  putting heavy work back on that task.

So: build this regardless of landing order; if #111 is already in, take the
measurement both as the #112 answer and as the #111 verification.

### 7. Other long-lived tasks — surveyed, deliberately not included

The firmware runs ~25 long-lived tasks (CAN rx/tx, obd_rx, elm327 ×2, csv_logger,
event_log writer, wifi_diag sampler, sleep, adc, autopid, smartconnect, slcan ×3,
comm_server ×3, uart ×3, sync_sys_time, led, datalog reaper, httpd, plus IDF's own).
Two already have this treatment (`sleep_task` via `/wake_probe`; the resume floor
check). Blanket-instrumenting the rest is gold-plating: their stacks are sized in our
own code where the depth is visible, none has a history of overflow, and each unused
field on a diagnostic endpoint is noise. `sys_evt` earns it because its stack is sized
by a Kconfig guess, its consumers are other people's callbacks, and it has actually
bricked a device. Rule for the future: a task gets a `/wake_probe` field and a floor
check when it has either overflowed once or its depth is not under this repo's
control — not before.

## What must NOT change (constraints)

- `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` stays **4608** in `sdkconfig` (and its
  `CONFIG_SYSTEM_EVENT_TASK_STACK_SIZE` mirror). This goal measures; it does not
  resize. Resizing is a separate, measurement-justified change (§5).
- No `event_log_emit` / `EVENT_LOG_DEBUG` call may be **added** anywhere that runs on
  `sys_evt` — in particular not inside the `wifi_diag_note_*` hooks
  (`wifi_diag.c:436-590`) and not in `wifi_mgr`'s event handling. The warning is
  emitted from the sampler task only.
- No new task, no new endpoint. The field rides `/wake_probe`; the check rides
  `wd_sampler_task`.
- Existing `/wake_probe` fields keep their names and meaning — tooling and the
  runbooks reference them.
- The seven existing emits in `wifi_diag.c` and the hazard comment at
  `wifi_diag.c:415-435` are #111's territory — do not touch them here (extend the
  comment with a pointer to the new floor constant if it reads better, but change no
  behavior).
- `components/event_log` stays a minimal leaf — no FreeRTOS task-inspection code goes
  in there.
- Nothing in the sleep/resume path changes.

## Acceptance criteria (each verifiable from the session transcript)

- **AC1 — build:** `idf.py build` exits 0 on the changed tree. Check: the build
  command and its final success line shown in the transcript.
- **AC2 — endpoint field exists in code:** `grep -n "sys_evt_stack_free" main/config_server.c`
  shows the new JSON field inside `wake_probe_handler`, and
  `grep -n "xTaskGetHandle(\"sys_evt\")" main/config_server.c` shows the handle
  lookup. Check: both grep outputs shown.
- **AC3 — floor + latch exist in the sampler, not the hooks:**
  `grep -n "WD_SYS_EVT_STACK_WARN_MIN_FREE" components/wifi_diag/wifi_diag.c` shows
  the constant defined with value 1024 and referenced from `wd_sample_once` (or a
  helper it calls), and `grep -n "event_log_emit" components/wifi_diag/wifi_diag.c`
  shows **no new** emit inside any `wifi_diag_note_*` function (same emit count and
  lines in those functions as before the change). Check: grep outputs shown, with the
  before/after emit sites accounted for.
- **AC4 — warning is latched:** the grep/read output in the transcript shows a
  boot-scoped `warned` flag guarding the emit (structure mirroring
  `sleep_mode.c:1485-1490`), so it can fire at most once per boot. Check: the guarded
  code block shown in the transcript.
- **AC5 — live on the bench device:** after OTA-flashing the build (via the
  `wican-ota` skill), `curl http://<device-ip>/wake_probe` returns valid JSON
  containing `"sys_evt_stack_free":N` with N > 0, alongside the pre-existing
  `sleep_task_stack_free`. Check: the curl command and full JSON response shown in
  the transcript.
- **AC6 — sdkconfig untouched:** `git diff --stat` (or `git status`) in the transcript
  shows `sdkconfig` is not among the changed files, and
  `grep -n "CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE" sdkconfig` still shows 4608.
  Check: both outputs shown.

## Manual checklist — explicitly OUTSIDE the /goal condition

Needs a human (or the router) in the loop; do not fold into the automated criteria:

- [ ] With the instrumented build flashed: set `debug=enabled`, reboot, let it
      connect. (Safe only on builds containing `0e05ffd` / stack 4608 — on anything
      older this boot-loops the device into safe-mode territory.)
- [ ] Force ≥1 real disconnect/reconnect (AP off ~10 s, or kick the client from the
      router). A boot-and-idle soak does not count — the first attempt after boot is
      debug-gated-off (`main/main.c:1179`).
- [ ] Read `sys_evt_stack_free` from `/wake_probe`; record the number in issue #112.
      That number is the answer #112 asks for: headroom = N, worst-case use = 4608 − N.
- [ ] Confirm no `sys_evt stack low` line appeared in `/event_log` during the exercise
      (if one did, that is a finding, not a test failure — see §5).
- [ ] Set `debug` back to disabled afterwards.
- [ ] Bench device is on TEST sleep values (14.5 V / 1 min) — restore normal sleep
      config before the car sees this device again, and keep sleep disabled while any
      flash is in progress (no flash interlock exists yet).
