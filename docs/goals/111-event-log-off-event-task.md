# Goal: move event-log formatting off the Wi-Fi event task (issue #111)

**/goal condition (ready to paste):**

```
/goal All acceptance criteria AC1-AC12 in docs/goals/111-event-log-off-event-task.md hold, each demonstrated in the transcript by its stated check, and no constraint C1-C9 is violated. The manual checklist at the bottom is explicitly NOT part of this condition.
```

---

## 0. What changed versus the previous version of this document

- **#112 shipped** (merged as `18a8eb3`): `/wake_probe` now reports `sys_evt_stack_free`
  (`main/config_server.c:2378,2384`, via the helper `task_stack_free_b()` at
  `config_server.c:2335`), and `wd_check_sys_evt_stack()`
  (`components/wifi_diag/wifi_diag.c:644-669`) warns once per boot below
  `WD_SYS_EVT_STACK_WARN_MIN_FREE` = 1024 (`wifi_diag.c:611`), called at the end of
  `wd_sample_once()` (`wifi_diag.c:766`).
- **We now have the real number.** With debug on and a forced AP outage that produced
  8 attempts, 3 drops and 2 associations in 45 s, `sys_evt` bottomed out at **2060 B free
  of 4608** — worst-case use **2548 B**. The old 2304 stack was 244 B too small; that is
  the boot loop, now measured instead of inferred. Idle references: 2668 free (debug off),
  2124 free (debug on, no reconnect). These numbers give §2.9 its targets.
- **New section 2.8** decides where the #112 stack guard lives after #111 (asked by an
  altitude review): it **stays in wifi_diag**, with its comment rewritten. Neither the
  warning nor the `/wake_probe` field relocates. The argument is in the section.
- **New section 2.9**: what the floor and the stack size mean after #111 — a decision
  rule, applied only after the post-#111 measurement.
- Line anchors refreshed for the file as it is after #112 (hooks moved by ~1–15 lines;
  sampler and handlers by ~60–80).
- New AC10–AC12 and constraints C8–C9; docs updates added to Stage 2.

## 1. The goal

The five `wifi_diag_note_*` hooks run on the ESP-IDF system event task (`sys_evt`). Today they
format strings there: `vsnprintf` into buffers, `snprintf` masking, and `event_log_emit()`, whose
`evl_vemit()` puts 328 bytes of buffers on the stack (`detail[112]` + `ts[24]` + `line[192]`,
`components/event_log/event_log.c:145-171`) and then calls `vsnprintf`, `localtime_r`, `strftime`
and `snprintf`. That overflowed the 2304-byte event task stack and boot-looped a real device
(issue #111). Commit `0e05ffd` raised the stack to 4608 as a mitigation only. The #112
measurement has since put a number on it: the deep path uses **2548 B** of that stack, so 2304
was 244 B short — the mechanism is no longer a theory.

After this change, the hooks record **bare facts** (an enum, a few ints, one short fixed SSID
buffer) into a small static ring, and a task with its own stack does all the formatting and
hands the finished line to event_log. The event task's stack stops being load-bearing for
logging.

**Important discovery that shapes the design:** `event_log_emit()` never touches the SD card on
the caller's thread — a writer task already drains the ring to SD
(`event_log.c:326`, `evl_writer_task`). Only the *formatting* runs on the caller. So we do not
build a second SD path; we defer the formatting, and we reuse everything else as-is.

## 2. The design

### 2.1 Where the deferral lives: inside `wifi_diag`, not inside `event_log`

An audit of every `event_log_emit()` caller (grep across the tree) shows the wifi_diag hooks are
the **only** callers on a system-owned stack. Everyone else — csv_logger, poll_log, sleep_mode,
can_wake, crash_report, config_server OTA, ncflash, datalog_lease_task, main — runs on a task
that owns its stack, sized by its author. No caller is in an ISR (and `evl_vemit` uses
`xSemaphoreGive`, the non-ISR form, so ISR use would be a bug anyway). Therefore:

- `event_log_emit()` keeps its contract and its callers unchanged.
- The fix is local to `components/wifi_diag/wifi_diag.c` plus **one small addition** to
  event_log (section 2.4).
- `event_log.h` gains a documented rule: *never call `event_log_emit` from the system event
  task, the esp_timer task, or an ISR; capture facts and defer* — so the next subsystem does
  not repeat this mistake.

### 2.2 The fact ring

A static ring of raw fact structs in `wifi_diag.c`, following the file's own idiom (static
array + `portMUX`, like `s_evt[]` at `wifi_diag.c:171`):

```c
typedef enum {
    WD_FACT_ATTEMPT, WD_FACT_CONNECTED, WD_FACT_GOT_IP,
    WD_FACT_DISCONNECTED, WD_FACT_BAN
} wd_fact_kind_t;

typedef struct {
    uint32_t seq;             // 1-based; 0 = slot never written (same scheme as s_evt)
    uint8_t  kind;            // wd_fact_kind_t
    uint8_t  reason;          // DISCONNECTED
    uint8_t  channel;         // CONNECTED
    uint8_t  bssid[6];        // CONNECTED
    bool     has_bssid;
    bool     link_was_up;     // DISCONNECTED: rssi/held are meaningful
    bool     emit_evl;        // DISCONNECTED: throttle verdict, decided at capture
    bool     evl_gated;       // ATTEMPT: debug flag was OFF at capture -> ring/timeline only
    int8_t   rssi_at_drop;    // DISCONNECTED
    uint32_t held_s;          // DISCONNECTED
    uint32_t suppressed;      // DISCONNECTED (throttle carry-over)
    uint32_t ttc_ms;          // GOT_IP (0 = no stopwatch)
    uint32_t ban_ms;          // BAN
    char     ssid[33];        // ATTEMPT/CONNECTED/DISCONNECTED/BAN; "" = none
    char     ip[16];          // GOT_IP
    struct timeval tv;        // wall clock AT THE EVENT
    int64_t  up_ms;           // uptime AT THE EVENT
} wd_fact_t;                  // ~100 bytes
```

- **Depth 16** (`#define WD_FACT_RING_N 16`): the measured worst burst was 8 attempts + 3
  drops + 2 associations in **45 seconds** (13 facts in 45 drain ticks); the ring only has to
  survive one drain interval (1 s), so 16 covers the real burst rate many times over.
- **Memory:** 16 × ~100 B ≈ 1.6 KB, static `.bss`, internal RAM (a static array in this file
  lands there; it is also the brick-safe choice — cannot fail to allocate). No heap use.
- **Lock:** reuse the module's existing `s_lock` (`wifi_diag.c:89`). Pushing a fact is one
  struct copy + two counter updates — within the file's stated rule for that lock ("a handful
  of scalar stores or one strlcpy"; a ~100-byte copy is the same order as the existing
  `wd_evt_push` strlcpy of 128 bytes).
- **Ring, not a FreeRTOS queue:** a queue would heap-allocate, add a second primitive, and give
  us blocking semantics we must not use anyway. The static ring matches `event_log`'s and
  `wifi_diag`'s existing pattern exactly.

### 2.3 What the five hooks become (capture)

Each hook (`wifi_diag_note_attempt` `:437`, `_connected` `:451`, `_got_ip` `:470`,
`_disconnected` `:506`, `_ban` `:583`) keeps its scalar bookkeeping (counters, stopwatch,
tally, throttle decision — all already plain stores under `s_lock`) and replaces **all** of:
`wd_evt_push()` (vsnprintf), `wd_mask_ssid()` (snprintf), `wifi_diag_reason_str()` use,
`event_log_emit()` / `EVENT_LOG_DEBUG()` — with: fill a `wd_fact_t` on the stack (~100 B, no
formatting), capture `gettimeofday(&f.tv)` + `f.up_ms` (cheap syscalls, no newlib formatting),
copy it into the ring under `s_lock`. Nothing else. Worst-case hook stack cost: the struct +
a few locals, ~150 bytes, zero library calls that format.

Two decisions stay at capture time because they are semantic, not cosmetic:

- **Debug gate (ATTEMPT):** the hook reads `event_log_debug_enabled()` (a volatile bool read)
  and stores `evl_gated = !enabled`. The fact is queued either way, because the attempt must
  still appear in the report timeline (`s_evt`), which was never debug-gated. Gate semantics
  are unchanged: an attempt that happens while debug is off never reaches the event log,
  exactly as `EVENT_LOG_DEBUG` behaves today.
- **Drop throttle (DISCONNECTED):** the `WD_EVL_REPEAT_MS` verdict and the `suppressed` count
  (`wifi_diag.c:537-550`) are already computed under `s_lock` in the hook; they stay there and
  ride in the fact (`emit_evl`, `suppressed`). Throttle behaviour is bit-for-bit identical.

### 2.4 Timestamps: one small event_log addition

The timestamp is captured **in the hook, at the moment of the event** (`f.tv`, `f.up_ms`).
To print it, event_log gets one new entry point:

```c
// Same as event_log_emit(), but with the caller-supplied event time instead of "now".
// For deferred emitters: capture tv/up_ms when the event happens, format later.
void event_log_emit_at(event_log_code_t code, const struct timeval *tv, int64_t up_ms,
                       const char *fmt, ...) __attribute__((format(printf, 4, 5)));
```

Implementation: `evl_vemit()` grows `tv`/`up_ms` parameters; when `tv == NULL` it does what it
does today (gettimeofday + esp_timer). `event_log_emit()` passes NULL — its behaviour and every
existing caller are untouched. The rendered line uses the **same format string**
`"%s up=%lldms %-12s %s"` (`event_log.c:170`) and the same `>= 2020 else "unsynced"` rule, so
the on-disk format is byte-identical. (If SNTP was not yet synced at the event, `tv` holds a
1970-era time and renders as "unsynced" — same output as today, where the emit ran at event
time.)

### 2.5 The drain (who formats, where)

`wd_drain_facts()`, a new function in `wifi_diag.c`. Loop: under `s_lock`, pop the oldest
undrained fact into a local copy (same seq-validation scheme as `evl_drain` at
`event_log.c:275-294`); outside the lock, do everything the hook used to do at format level:

- `wd_evt_push()` the timeline line (using the fact's `up_ms/1000` as `up_s`, so the report
  timeline keeps event-time stamps — `wd_evt_push` gains an explicit `up_s` parameter or a
  variant; today it reads the clock itself, `wifi_diag.c:330-348`).
- `wd_mask_ssid()` where the old code masked (attempt, connected, ban). **The raw SSID must
  still never reach an event_log line** — see constraint C4.
- `event_log_emit_at(EVL_WIFI, &f.tv, f.up_ms, ...)` with the exact same format strings and
  gating/throttle verdicts the fact carries.

**Primary drainer: the existing `wd_sampler_task`** (`wifi_diag.c:769`, prio 2, 1 Hz). One call
to `wd_drain_facts()` per loop iteration, before `wd_sample_once()`. No new task, no new stack
allocation, no new priority to reason about. Latency: at most ~1 s from event to formatted
line, which is fine for a forensic log whose printed timestamp is the event time anyway.

**Its stack grows:** `WD_TASK_STACK` (`wifi_diag.c:60`) 3584 → **4608**. The drain adds
`wd_fact_t` copy (~100 B) + mask buffer (48) + `wd_evt_push` text (128 + vsnprintf) +
`event_log_emit_at` (328 + newlib), on top of `wd_sample_once`'s existing
`wifi_config_t`/`wifi_ap_record_t` locals. ~1 KB extra internal RAM; measure the real
high-water on the bench (manual checklist) before trusting it long-term.

**Backup drainer: the wifi_diag HTTP handlers.** `wd_send_report` (`:1027`) and `wd_send_json`
(`:1313`) call `wd_drain_facts()` first. This closes the one availability gap: today, if the
sampler task fails to create, the hooks still record everything (`wifi_diag.c:787-792` says
so); with the drain living only in the sampler, a dead sampler would silence the timeline and
the event-log lines. With the handler drain, evidence appears whenever anyone looks, on the
httpd task's big stack. The pop-copy-under-lock scheme makes two concurrent drainers safe:
each fact is claimed by exactly one popper. (The rejected alternatives — event_log writer
task, esp_timer callback — are re-examined in §2.8 and still lose.)

### 2.6 Full ring behaviour

**Never block — overwrite the oldest** (the writer is the system event task; blocking or even
retrying there is exactly the failure class we are removing). Same wrap accounting as
`evl_drain` (`event_log.c:230-239`): a monotonic `s_fact_dropped` counter increments for every
fact overwritten before it was drained. Drops are made visible twice:

- `s_fact_dropped` is added to the `/wifi_diag/json` payload (additive field — allowed).
- When a drain pass observes the counter increased, it emits one
  `event_log_emit(EVL_WARN, "wifi_diag: %u link events lost before formatting")` line from the
  drainer's own stack, throttled to at most one per 60 s (reuse the `WD_EVL_REPEAT_MS`
  pattern), so a pathological burst cannot flood events.log.

Realistically 16 deep at a 1 Hz drain never fills: the measured worst burst averaged one fact
per 3.5 s; it would take >16 link events inside one second.

### 2.7 What this does to the ungated emits

All seven emits in `wifi_diag.c` (`:448 :467 :496 :502 :570 :576 :595`) move behind the fact
ring, so **every** device — debug on or off — stops paying formatting costs on `sys_evt`. The
event task's remaining logging cost per event is one ~100-byte struct copy under a brief
critical section. The `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4608` mitigation **stays** in
this change as a belt (constraint C6); §2.9 gives the rule for touching it later.

The hazard comment at `wifi_diag.c:415-435` is rewritten, not deleted: its new job is to say
*"these hooks run on sys_evt; they may capture facts and nothing else — no formatting, no
emits; the tripwire for this rule is `wd_check_sys_evt_stack()` below."* The matching section
of `docs/internals/wifi_diag.md` ("The sampler also watches someone else's stack", starting
line 163) opens with a sentence that becomes false after this change ("Every
`event_log_emit()` in this file runs on `sys_evt`") and is updated in the same commit.

### 2.8 Where the #112 stack guard lives after #111: it stays in wifi_diag

An altitude review argued: wifi_diag is the right home for `wd_check_sys_evt_stack()` today
because wifi_diag is the component *spending* the bytes on `sys_evt`; #111 dissolves that
pairing, so the check becomes a task-inspection routine parked in a Wi-Fi leaf for no reason.
The review's conclusion was to move it, perhaps to a new `task_health` component. **We
examined that and decided the check stays.** Three reasons, in order of weight:

1. **The review's premise is not quite true.** After #111, wifi_diag still runs code on
   `sys_evt` — the capture path: a ~100 B struct fill, `gettimeofday`, and a ring copy inside
   a `portMUX` critical section, in all five hooks. It is small by design, and *keeping it
   small is a rule someone will one day be tempted to break* ("just one quick emit here, the
   fact ring is overkill for my line"). The check is the tripwire for exactly that
   temptation, and the most likely regression site is wifi_diag's own hooks. The component
   that owns the temptation should own the alarm. This is a stronger pairing than
   "wifi_diag spends the bytes", and it survives #111.
2. **Every other home either buys a new task or lands on a rejected host.** The guard must
   run periodically on a task that owns its stack, with no HTTP polling (constraint C8 keeps
   that property). The candidates:
   - **New `task_health` component:** a new component boundary, a new task, a new ~2–3 KB
     internal-RAM stack, a {name, floor} table with **one row** — bought to run one
     comparison per second. #112's own survey (its §7) set the rule: a task earns a floor
     check when it has overflowed once or its depth is outside this repo's control — that is
     one task today. Building the generalized component now is the gold-plating that rule
     exists to prevent. **The rule for later:** the day a *second* task qualifies, create
     `task_health` then and move both rows; the migration is one static function, two
     statics and a `#define` — cheap precisely because we did not generalize it early.
   - **`main/`:** allowed by the dependency rules, but main has no periodic own-stack task
     to ride. The sleep loop is off-limits (nothing in sleep/resume changes, C7), the LED
     task is a worse taxonomy than wifi_diag, and a new task in main pays the same RAM as
     `task_health` without even the component boundary to show for it.
   - **The #111 drain's home:** #111 creates no new home — the drain rides `wd_sampler_task`
     inside wifi_diag. This option collapses into "stay in wifi_diag."

   So "stay" is not inertia; it is the only option that costs nothing and violates nothing.
3. **The drain and the guard belong on the same task.** After #111 the sampler tick is:
   drain facts (format on my own stack) → sample link → check `sys_evt` headroom. One
   low-priority task owns both halves of the sys_evt story: *doing the work somewhere safe*
   and *verifying the unsafe place stayed cheap*. Splitting the guard out would separate it
   from the hazard comment, the fact ring, and the file where the boot-loop history is
   written down.

What actually changes in #111: the comment block above `wd_check_sys_evt_stack()`
(`wifi_diag.c:601-643`) is rewritten to carry the *new* justification (tripwire for the
capture-only rule; regression guard for anything anyone hangs on `sys_evt`, not only
wifi_diag), because its current text leans on "one more emit on this stack" wording that
describes the pre-#111 world.

**The `/wake_probe` piece never had a relocation question.** `task_stack_free_b()` and the
`sys_evt_stack_free` field live in `main/config_server.c` (`:2335`, `:2378`, `:2384`) —
main's endpoint, main's helper, main may call FreeRTOS freely. It stays byte-for-byte
untouched. So the answer to "move together or separately" is: **neither piece moves.**

**The two previously rejected drain hosts stay rejected — and get stronger:**

- **event_log writer task:** it deliberately does not start on crash-guard skip boots
  (`event_log.c:370-385`) — exactly the post-crash boots where Wi-Fi evidence and the stack
  guard matter most. Parking either the drain or the guard there would silence them on the
  boots that follow the very failure class they exist to catch.
- **esp_timer callback:** it runs on the shared esp_timer system task — another
  Kconfig-sized stack this repo does not own. An emit from there costs the same ~800 B on a
  sibling of the task that already boot-looped. That is the original bug with a different
  task name.

Since no new home is being created, neither rejection weakens; §2.8's decision removes the
last reason to revisit them.

Resume-in-place note: this firmware resumes on wake without rebooting, so the boot-scoped
statics (`s_sys_evt`, `s_sys_evt_stack_warned`, the fact ring, the drop counter) all survive
a wake. That is correct here, not a bug: the high-water mark is monotone for the life of the
task and the task survives light sleep, so the latch firing at most once per *power cycle*
is the honest behaviour. Nothing to change.

### 2.9 The floor and the stack size after #111: a decision rule, not a number

Today's floor (1024) means "one more ~800 B emit no longer fits." After #111 no emit runs on
`sys_evt` at all, so the floor's meaning changes to **"someone put heavy work back on this
task."** The measured numbers expose a problem with keeping 1024 blindly: worst-case free was
2060 *with* the formatting still on the stack; #111 removes roughly the ~800 B emit path, so
post-#111 worst-case free should land around **2700–2900**. From there, one re-added emit
(~800 B) drops free to ~2000 — **still far above 1024, so the tripwire would never fire on
the very regression it exists to catch.** A floor that cannot fire is decoration.

The rule (apply *after* the post-#111 bench measurement, never before):

- Let **F** = measured post-#111 `sys_evt_stack_free`, taken with the same procedure that
  produced 2060 (debug on, forced multi-attempt reconnect burst).
- The floor must satisfy both: **floor > F − 800** (one re-added emit fires it) and
  **floor ≤ F − 512** (headroom for deep `sys_evt` paths the bench burst did not exercise —
  other components' handlers, WPA edge cases — so no false alarm).
- Pick a round number inside that window. With the predicted F ≈ 2800 the window is
  (2000, 2288] and **2048** is the natural choice — but the measurement decides, not this
  prediction. If the window is empty (F < ~1300), #111 did not remove what we thought;
  stop and investigate instead of tuning the floor.
- The retighten is its **own commit, after the measurement**, citing the measured F in the
  constant's comment — the floor is never changed in the same commit that changes the code
  it guards (see §5).

**The stack stays 4608 in and after #111 (constraint C6).** Lowering it toward the IDF
default 2304 is now *discussable* but still **recommended against**: predicted post-#111
worst-case use is ~1750–1900, so 2304 would leave only ~400–550 free — under any sane floor.
The reclaimable RAM is 1–2 KB against ~32 KB free, and the failure mode of guessing short is
the boot loop this whole effort exists to bury. If internal RAM ever becomes genuinely
scarce, the rule is: new size ≥ (4608 − F) + floor + 512, rounded up to a 256 multiple, then
re-measure on the resized build with the same burst procedure and confirm free ≥ floor
before trusting it. Until someone needs those bytes, keep 4608 and let the retightened floor
be the safety mechanism — the floor is free; the risk is not.

## 3. Scope boundaries — what must NOT change

- **C1 — on-disk event-log line format.** The format string `"%s up=%lldms %-12s %s"` and the
  "unsynced" rule are unchanged; existing tooling that parses events.log keeps working.
- **C2 — HTTP surface.** `/event_log`, `/event_log/ram`, `/event_log/status`,
  `/wifi_diag/*`, `/wake_probe` routes and their existing fields are unchanged — existing
  field names keep their spelling and meaning (tooling and runbooks reference them). New
  JSON fields may be **added** (the drop counter); none removed or renamed.
- **C3 — `EVENT_LOG_DEBUG` semantics.** The macro (`event_log.h:139`) is untouched; it still
  does not evaluate its arguments when gated. The deferred attempt line is gated by the
  flag's value **at the moment of the event**, matching today.
- **C4 — SSID/BSSID masking.** No raw SSID or full BSSID may ever be formatted into an
  event_log line or into `wd_evt_t.text` (the privacy invariant documented at
  `wifi_diag.c:154-168` and `:460-464`). The fact struct carries them raw only inside RAM,
  exactly as `s_evt` already does; masking happens at format time in the drain.
- **C5 — `event_log_emit()` contract and all non-wifi_diag callers.** No other subsystem's
  call sites change. `event_log_emit()` still formats on the caller and never blocks.
- **C6 — sdkconfig.** `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4608` is not reduced here
  (§2.9 gives the only path by which it may ever change, and it is not this change).
- **C7 — no new tasks, no heap allocations** in the new mechanism, and nothing in the
  sleep/resume path changes. Static `.bss` only. Internal-RAM budget for the whole change:
  ~1.6 KB ring + ~1 KB sampler stack raise ≈ 2.6 KB, against ~32 KB free — acceptable, and
  no dynamic allocation can fail at runtime.
- **C8 — the #112 guard keeps working, continuously, without HTTP polling.**
  `wd_check_sys_evt_stack()` stays in `components/wifi_diag/wifi_diag.c`, called from the
  sampler tick; `task_stack_free_b()` and the `sys_evt_stack_free` field stay in
  `main/config_server.c` untouched. No commit in the series may leave the check absent,
  disabled, or moved (§2.8 is the decision record; §5 the ordering proof).
- **C9 — the floor value changes only against a cited measurement.**
  `WD_SYS_EVT_STACK_WARN_MIN_FREE` stays **1024** throughout the #111 code commits. The
  retighten (§2.9) is a separate follow-up commit whose comment cites the measured
  post-#111 `sys_evt_stack_free`.

## 4. Risks, ranked

1. **Wi-Fi lines captured but not yet drained are lost in a crash** (≤ ~1 s window). Before,
   the line reached the event_log RAM ring synchronously — when it didn't panic the device
   outright. The crash reporter itself is unaffected (it emits after reboot from RTC data,
   `components/crash_report/crash_report.c:127`). Accepted trade: a 1-second forensic gap
   versus a boot loop. Nothing else on this device depends on wifi lines being synchronous.
2. **Sleep/wake:** the radio is off during sleep, so no facts are produced; facts queued just
   before sleep drain after resume with correct event-time stamps. Verify the sampler task is
   not deleted by the resume-in-place path (it is not today; keep it that way). Boot-scoped
   statics surviving a wake is correct behaviour here (§2.8, last paragraph).
3. **File ordering skew:** a wifi line can land in events.log up to ~1 s after a line from
   another subsystem that happened later. The printed timestamp is the event time, so the
   truth is preserved; the file is merely not strictly append-ordered across subsystems.
   Cosmetic; document it in a comment at the drain.
4. **Sampler stack raise is a guess** (3584→4608, reasoned not measured). Same remedy as
   #112: check `uxTaskGetStackHighWaterMark` on the bench before trusting it.
5. **Two concurrent drainers** (sampler + HTTP) interleaving: made safe by claim-under-lock
   pop; worst case is two wifi lines swapping order within the same second. Verify the pop
   marks the slot consumed (seq check like `evl_drain`) so a fact cannot be emitted twice.
6. **A floor that cannot fire** if the retighten step is skipped: with 4608 and post-#111
   free ≈ 2800, the 1024 floor no longer catches a single re-added emit. Not a device risk,
   but the guard silently degrades to decoration — the manual checklist carries the
   retighten so it is not forgotten.
7. **Debug-toggle edge:** enabling debug shows attempts from the *next* event onward; up to
   one already-queued attempt from the prior second stays gated. Negligible and matches the
   "at the moment of the event" semantics.

## 5. Staged plan (ordering is part of the safety argument)

The guard (`wd_check_sys_evt_stack`) is never moved, so there is no window where the hazard
exists without its tripwire — the only ordering rules left are *within* the rewrite:

- **Stage 1 — event_log:** add `event_log_emit_at()`; thread `tv/up_ms` through `evl_vemit()`
  with NULL = today's behaviour; add the "never from sys_evt / esp_timer / ISR" contract note
  to `event_log.h`. Build clean. Zero behaviour change for existing callers. The guard is
  untouched and live.
- **Stage 2 — wifi_diag:** fact ring + capture in the five hooks + drain in the sampler and
  the two HTTP handlers + `WD_TASK_STACK` 4608 + drop counter in JSON. In the same commit:
  rewrite the hazard comment (`:415-435`) and the guard's comment block (`:601-643`) per
  §2.7/§2.8, and update `docs/internals/wifi_diag.md` §"The sampler also watches someone
  else's stack". `wd_check_sys_evt_stack()` itself and
  `WD_SYS_EVT_STACK_WARN_MIN_FREE=1024` change **only in comments**; the call at the end of
  `wd_sample_once()` stays the tick's last action. Build clean.
- **Stage 3 — bench (no car needed):** flash over OTA; confirm boot-time `associated`/`got
  IP` lines appear via the new path; then enable `debug` **only after** the fixed build is
  running (it boot-looped the old one — have the safe-mode rescue recipe ready), reboot,
  force or wait for a reconnect, soak ≥ 15 min with debug on. Take the post-#111
  measurement (manual checklist) — this is both the #111 proof and the #112 §5 "only after
  #111" input.
- **Stage 4 — floor retighten (separate commit, after Stage 3's number exists):** apply the
  §2.9 window rule to the measured F; update `WD_SYS_EVT_STACK_WARN_MIN_FREE` with the
  measurement cited in its comment. Never folded into Stage 2 (constraint C9). The stack
  size is not touched (C6).

## 6. Acceptance criteria (each verifiable from the transcript)

- **AC1 — build:** `idf.py build` exits 0. Check: the command's final output shown in the
  transcript (e.g. "Project build complete").
- **AC2 — no formatting in the hooks:** the five functions `wifi_diag_note_attempt`,
  `_connected`, `_got_ip`, `_disconnected`, `_ban` contain no calls to `event_log_emit`,
  `EVENT_LOG_DEBUG`, `wd_evt_push`, `wd_mask_ssid`, `snprintf`, `vsnprintf`, `strftime` or
  `localtime_r`. Check: Read output of the five hook bodies shown in the transcript, plus
  `grep -n "event_log_emit\|EVENT_LOG_DEBUG" components/wifi_diag/wifi_diag.c` showing hits
  only inside `wd_drain_facts` (including its drop warning) and `wd_check_sys_evt_stack`.
- **AC3 — deferred emit API exists:** check: grep shows `event_log_emit_at` declared in
  `components/event_log/include/event_log.h` and defined in `event_log.c`.
- **AC4 — line format unchanged:** check: grep shows the exact string
  `up=%lldms` appearing in `event_log.c` in the single line-assembly `snprintf`, unchanged.
- **AC5 — timestamp at event time:** check: Read output shows the hooks capturing
  `gettimeofday`/`esp_timer_get_time` into the fact, and the drain passing `&f.tv, f.up_ms`
  to `event_log_emit_at`.
- **AC6 — no blocking, drop-oldest:** check: Read output of the fact-push shows no
  `xQueueSend`/`xSemaphoreTake`/delay on the capture path, and a dropped-count increment on
  overwrite; grep shows the drop counter reported in the `/wifi_diag` JSON assembly.
- **AC7 — static internal memory only:** check: Read output shows the fact ring as a
  `static` array (no `heap_caps_malloc`/`malloc` in the new mechanism).
- **AC8 — no other callers touched:** check:
  `git diff --stat` shown in the transcript touches only `components/event_log/*`,
  `components/wifi_diag/*`, `docs/*`, and (if the JSON field needs it) `main/wifi_mgr.c` —
  no csv_logger/poll_log/sleep_mode/can_wake/crash_report changes, and **no diff hunks in
  `main/config_server.c`** (the /wake_probe piece does not move, §2.8).
- **AC9 — device proof (bench, transcript-visible):** after OTA-flashing the build to the
  bench device, `curl http://<device-ip>/event_log/ram` output shown in the transcript
  contains a `WIFI` `associated ssid=` line and a `got IP` line with a plausible timestamp,
  produced by the deferred path (this exercises capture → drain → emit end-to-end on a boot
  connect). The wican-ota skill covers the flash-and-verify procedure.
- **AC10 — guard present at every commit boundary:** check: for **each** commit in the
  series, `git grep -n "wd_check_sys_evt_stack" <sha> -- components/wifi_diag/wifi_diag.c`
  shown in the transcript returns both the definition and the call site (the function
  exists and is called at every step), and the final tree's Read output shows the call
  still the last action of `wd_sample_once()`.
- **AC11 — comments and internals doc tell the post-#111 truth:** check: Read/grep output
  shows (a) the rewritten hooks comment states hooks may capture facts only, no
  formatting/emits on `sys_evt`; (b) the guard's comment block carries the §2.8 tripwire
  justification; (c) `docs/internals/wifi_diag.md` no longer claims that every
  `event_log_emit()` in `wifi_diag.c` runs on `sys_evt` (grep for
  `Every \`event_log_emit()\` in this file` returns no match).
- **AC12 — floor and endpoint untouched by the code commits:** check:
  `grep -n "WD_SYS_EVT_STACK_WARN_MIN_FREE" components/wifi_diag/wifi_diag.c` shows the
  value still **1024**, and `grep -n "sys_evt_stack_free\|task_stack_free_b" main/config_server.c`
  shows the same lines as before the change (helper at `:2335`, field in the
  `wake_probe_handler` body). The Stage 4 retighten commit is explicitly **not** part of
  this /goal condition — it depends on the manual measurement below.

## 7. Manual checklist — OUTSIDE the /goal condition

These need a human at the bench (AP manipulation / long observation):

- [ ] With the fixed build running, set `debug=enabled` (safe-mode rescue recipe at hand),
      reboot: device must come up normally and stay up >= 15 min. This is the exact scenario
      that boot-looped v1.19.1.
- [ ] Power-cycle the access point (or move the device out of range and back) with debug
      on: an `attempt ssid=` line appears in `/event_log` — the line that has never once
      been successfully written by any device.
- [ ] **The #111 proof-of-work measurement:** repeat the #112 procedure (debug on, forced
      AP outage producing a multi-attempt burst), then read `sys_evt_stack_free` from
      `/wake_probe`. Pre-#111 worst case was **2060 free of 4608**; expect roughly
      **2700–2900**. Pass line: strictly **> 2060**, and ideally >= 2560 (>= 500 B
      recovered). If it is not materially above 2060, #111 did not remove what we thought —
      investigate before any floor change. Record the number (F) in issues #111 and #112.
- [ ] **Floor retighten (Stage 4):** apply §2.9's window rule to F — floor in
      (F − 800, F − 512], round number, own commit, measurement cited in the comment.
- [ ] Confirm the DROP throttle still behaves: unplug the AP for several minutes; at most
      one DROP line per identical reason per 60 s in `/event_log`, with `[+N similar
      suppressed]` carried on the next line through.
- [ ] Sleep/wake pass: let the device sleep and wake (bench sleep config); wifi lines
      around the transition carry event-time stamps and none are duplicated.
      (Resume-in-place: the latch and ring surviving the wake is correct, §2.8.)
- [ ] Read the sampler task's stack high-water mark after the soak; confirm comfortable
      headroom under the new 4608.
- [ ] Restore normal `debug` setting and the bench sleep values (bench is on TEST values
      14.5 V / 1 min) afterwards, and keep sleep disabled while any flash is in progress —
      no flash interlock exists yet.
