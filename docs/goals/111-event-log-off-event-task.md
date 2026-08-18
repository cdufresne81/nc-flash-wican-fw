# Goal: move event-log formatting off the Wi-Fi event task (issue #111)

**/goal condition (ready to paste):**

```
/goal All acceptance criteria AC1-AC9 in docs/goals/111-event-log-off-event-task.md hold, each demonstrated in the transcript by its stated check, and no constraint C1-C7 is violated.
```

---

## 1. The goal

The five `wifi_diag_note_*` hooks run on the ESP-IDF system event task (`sys_evt`). Today they
format strings there: `vsnprintf` into buffers, `snprintf` masking, and `event_log_emit()`, whose
`evl_vemit()` puts 328 bytes of buffers on the stack (`detail[112]` + `ts[24]` + `line[192]`,
`components/event_log/event_log.c:145-171`) and then calls `vsnprintf`, `localtime_r`, `strftime`
and `snprintf`. That overflowed the 2304-byte event task stack and boot-looped a real device
(issue #111). Commit `0e05ffd` raised the stack to 4608 as a mitigation only.

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

- `event_log_emit()` keeps its contract and its callers unchanged (answer to question 5).
- The fix is local to `components/wifi_diag/wifi_diag.c` plus **one small addition** to
  event_log (section 2.4).
- `event_log.h` gains a documented rule: *never call `event_log_emit` from the system event
  task, the esp_timer task, or an ISR; capture facts and defer* — so the next subsystem does
  not repeat this mistake.

### 2.2 The fact ring (questions 1, 2)

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

- **Depth 16** (`#define WD_FACT_RING_N 16`): a reconnect loop produces an attempt + a drop
  every few seconds and the drain runs at 1 Hz, so 16 covers a burst several times over.
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

Each hook (`wifi_diag_note_attempt` `:436`, `_connected` `:450`, `_got_ip` `:469`,
`_disconnected` `:505`, `_ban` `:582`) keeps its scalar bookkeeping (counters, stopwatch,
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
  (`wifi_diag.c:537-549`) are already computed under `s_lock` in the hook; they stay there and
  ride in the fact (`emit_evl`, `suppressed`). Throttle behaviour is bit-for-bit identical.

### 2.4 Timestamps: one small event_log addition (question 4)

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
  variant; today it reads the clock itself at `wifi_diag.c:338`).
- `wd_mask_ssid()` where the old code masked (attempt, connected, ban). **The raw SSID must
  still never reach an event_log line** — see constraint C4.
- `event_log_emit_at(EVL_WIFI, &f.tv, f.up_ms, ...)` with the exact same format strings and
  gating/throttle verdicts the fact carries.

**Primary drainer: the existing `wd_sampler_task`** (`wifi_diag.c:694`, prio 2, 1 Hz). One call
to `wd_drain_facts()` per loop iteration, before `wd_sample_once()`. No new task, no new stack
allocation, no new priority to reason about (answer to "who owns the worker": wifi_diag owns
it, and it already exists). Latency: at most ~1 s from event to formatted line, which is fine
for a forensic log whose printed timestamp is the event time anyway.

**Its stack grows:** `WD_TASK_STACK` 3584 → **4608**. The drain adds `wd_fact_t` copy (~100 B)
+ mask buffer (48) + `wd_evt_push` text (128 + vsnprintf) + `event_log_emit_at` (328 +
newlib), on top of `wd_sample_once`'s existing `wifi_config_t`/`wifi_ap_record_t` locals.
~1 KB extra internal RAM; the sibling issue #112's measurement pattern can confirm the real
high-water later.

**Backup drainer: the wifi_diag HTTP handlers.** `wd_send_report` (`:952`) and `wd_send_json`
(`:1238`) call `wd_drain_facts()` first. This closes the one availability gap: today, if the
sampler task fails to create, the hooks still record everything (`wifi_diag.c:713-717` says
so); with the drain living only in the sampler, a dead sampler would silence the timeline and
the event-log lines. With the handler drain, evidence appears whenever anyone looks, on the
httpd task's big stack. The pop-copy-under-lock scheme makes two concurrent drainers safe:
each fact is claimed by exactly one popper. (Why not the event_log writer task via a callback:
that task deliberately does not start on crash-guard skip boots, `event_log.c:370-385` — which
are exactly the post-crash boots where Wi-Fi evidence matters most. Why not an esp_timer
callback: that runs on the shared esp_timer system task, which is the same class of mistake
this issue exists to remove.)

### 2.6 Full ring behaviour (question 3)

**Never block — overwrite the oldest** (the writer is the system event task; blocking or even
retrying there is exactly the failure class we are removing). Same wrap accounting as
`evl_drain` (`event_log.c:230-239`): a monotonic `s_fact_dropped` counter increments for every
fact overwritten before it was drained. Drops are made visible twice:

- `s_fact_dropped` is added to the `/wifi_diag/json` payload (additive field — allowed).
- When a drain pass observes the counter increased, it emits one
  `event_log_emit(EVL_WARN, "wifi_diag: %u link events lost before formatting")` line from the
  drainer's own stack, throttled to at most one per 60 s (reuse the `WD_EVL_REPEAT_MS`
  pattern), so a pathological burst cannot flood events.log.

Realistically 16 deep at a 1 Hz drain never fills: it would take >16 link events inside one
second.

### 2.7 What this does to the ungated emits

All seven emits in `wifi_diag.c` (`:447 :466 :495 :501 :569 :575 :594`) move behind the fact
ring, so **every** device — debug on or off — stops paying formatting costs on `sys_evt`. The
event task's remaining logging cost per event is one ~100-byte struct copy under a brief
critical section. The `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4608` mitigation **stays** in
this change as a belt (constraint C6); shrinking it back is #112's decision, made against a
measured high-water mark, not here.

## 3. Scope boundaries — what must NOT change (question 6)

- **C1 — on-disk event-log line format.** The format string `"%s up=%lldms %-12s %s"` and the
  "unsynced" rule are unchanged; existing tooling that parses events.log keeps working.
- **C2 — HTTP surface.** `/event_log`, `/event_log/ram`, `/event_log/status`,
  `/wifi_diag/*` routes and their existing fields are unchanged. New JSON fields may be
  **added** (the drop counter); none removed or renamed.
- **C3 — `EVENT_LOG_DEBUG` semantics.** The macro (`event_log.h:139`) is untouched; it still
  does not evaluate its arguments when gated. The deferred attempt line is gated by the flag's
  value **at the moment of the event**, matching today.
- **C4 — SSID/BSSID masking.** No raw SSID or full BSSID may ever be formatted into an
  event_log line or into `wd_evt_t.text` (the privacy invariant documented at
  `wifi_diag.c:154-168` and `:459-463`). The fact struct carries them raw only inside RAM,
  exactly as `s_evt` already does; masking happens at format time in the drain.
- **C5 — `event_log_emit()` contract and all non-wifi_diag callers.** No other subsystem's
  call sites change. `event_log_emit()` still formats on the caller and never blocks.
- **C6 — sdkconfig.** `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4608` is not reduced here.
- **C7 — no new tasks, no heap allocations** in the new mechanism. Static `.bss` only.
  Internal-RAM budget for the whole change: ~1.6 KB ring + ~1 KB sampler stack raise
  ≈ 2.6 KB, against ~32 KB free — acceptable, and no dynamic allocation can fail at runtime.

## 4. Risks, ranked (question 7)

1. **Wi-Fi lines captured but not yet drained are lost in a crash** (≤ ~1 s window). Before,
   the line reached the event_log RAM ring synchronously — when it didn't panic the device
   outright. The crash reporter itself is unaffected (it emits after reboot from RTC data,
   `components/crash_report/crash_report.c:127`). Accepted trade: a 1-second forensic gap
   versus a boot loop. Nothing else on this device depends on wifi lines being synchronous.
2. **Sleep/wake:** the radio is off during sleep, so no facts are produced; facts queued just
   before sleep drain after resume with correct event-time stamps. Verify the sampler task is
   not deleted by the resume-in-place path (it is not today; keep it that way).
3. **File ordering skew:** a wifi line can land in events.log up to ~1 s after a line from
   another subsystem that happened later. The printed timestamp is the event time, so the
   truth is preserved; the file is merely not strictly append-ordered across subsystems.
   Cosmetic; document it in a comment at the drain.
4. **Sampler stack raise is a guess** (3584→4608, reasoned not measured). Same remedy as #112:
   check `uxTaskGetStackHighWaterMark` on the bench before trusting it.
5. **Two concurrent drainers** (sampler + HTTP) interleaving: made safe by claim-under-lock
   pop; worst case is two wifi lines swapping order within the same second. Verify the pop
   marks the slot consumed (e.g. seq check like `evl_drain`) so a fact cannot be emitted twice.
6. **Debug-toggle edge:** enabling debug shows attempts from the *next* event onward; up to
   one already-queued attempt from the prior second stays gated. Negligible and matches the
   "at the moment of the event" semantics.

## 5. Staged plan (question 8)

- **Stage 1 — event_log:** add `event_log_emit_at()`; thread `tv/up_ms` through `evl_vemit()`
  with NULL = today's behaviour; add the "never from sys_evt / esp_timer / ISR" contract note
  to `event_log.h`. Build clean. Zero behaviour change for existing callers.
- **Stage 2 — wifi_diag:** fact ring + capture in the five hooks + drain in the sampler and
  the two HTTP handlers + `WD_TASK_STACK` 4608 + drop counter in JSON. Build clean.
- **Stage 3 — bench (no car needed):** flash over OTA; confirm boot-time `associated`/`got IP`
  lines appear via the new path; then enable `debug` **only after** the fixed build is
  running (it boot-looped the old one — have the safe-mode rescue recipe ready), reboot, force
  or wait for a reconnect, and look for the historic first `attempt ssid=` line. Soak ≥ 15 min
  with debug on. Then #112's measurement can be taken with real numbers.

## 6. Acceptance criteria (each verifiable from the transcript)

- **AC1 — build:** `idf.py build` exits 0. Check: the command's final output shown in the
  transcript (e.g. "Project build complete").
- **AC2 — no formatting in the hooks:** the five functions `wifi_diag_note_attempt`,
  `_connected`, `_got_ip`, `_disconnected`, `_ban` contain no calls to `event_log_emit`,
  `EVENT_LOG_DEBUG`, `wd_evt_push`, `wd_mask_ssid`, `snprintf`, `vsnprintf`, `strftime` or
  `localtime_r`. Check: Read output of the five hook bodies shown in the transcript, plus
  `grep -n "event_log_emit\|EVENT_LOG_DEBUG" components/wifi_diag/wifi_diag.c` showing hits
  only inside the drain function.
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
- **AC7 — static internal memory only:** check: Read output shows the fact ring as a `static`
  array (no `heap_caps_malloc`/`malloc` in the new mechanism).
- **AC8 — no other callers touched:** check:
  `git diff --stat` shown in the transcript touches only `components/event_log/*`,
  `components/wifi_diag/*`, and (if the JSON field needs it) `main/wifi_mgr.c` — no
  csv_logger/poll_log/sleep_mode/can_wake/crash_report changes.
- **AC9 — device proof (bench, transcript-visible):** after OTA-flashing the build to the
  bench device, `curl http://<device-ip>/event_log/ram` output shown in the transcript
  contains a `WIFI` `associated ssid=` line and a `got IP` line with a plausible timestamp,
  produced by the deferred path (this exercises capture → drain → emit end-to-end on a boot
  connect). The wican-ota skill covers the flash-and-verify procedure.

## 7. Manual checklist — OUTSIDE the /goal condition

These need a human at the bench (AP manipulation / long observation):

- [ ] With the fixed build running, set `debug=enabled` (safe-mode rescue recipe at hand),
      reboot: device must come up normally and stay up ≥ 15 min. This is the exact scenario
      that boot-looped v1.19.1.
- [ ] Power-cycle the access point (or move the device out of range and back) with debug on:
      an `attempt ssid=` line appears in `/event_log` — the line that has never once been
      successfully written by any device.
- [ ] Confirm the DROP throttle still behaves: unplug the AP for several minutes; at most one
      DROP line per identical reason per 60 s in `/event_log`, with `[+N similar suppressed]`
      carried on the next line through.
- [ ] Sleep/wake pass: let the device sleep and wake (bench sleep config); wifi lines around
      the transition carry event-time stamps and none are duplicated.
- [ ] Read the sampler task's stack high-water mark after the soak; confirm comfortable
      headroom under the new 4608.
- [ ] Restore normal `debug` setting and the bench sleep values afterwards.
