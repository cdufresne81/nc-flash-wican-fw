# Goal document: erase-edge keepalive (protect NC Flash 2.12.0 hosts)

Designed by Fable. Verified against the code at 000eba8 (firmware) and 575b3c0 / 575b3c0~1
(host 2.13.0 / 2.12.0).

**TLDR of the design:** send a small `NCFWPROG` heartbeat every 5 seconds while the firmware
waits for the ECU at the erase edges and at TransferExit, using zero-wait queue sends so the
heartbeat can never eat the ECU's time budget and its failure can never abort the flash. And add
one broader rule the analysis showed is actually the bigger safety win: once the erase can have
started ("point of no return"), NO progress-line failure may abort the flash any more — the
firmware finishes the ECU on its own and reports the lost host afterwards. Today a 2-second WiFi
hiccup at block 500 bricks the ECU even with matched versions; this design closes that hole too.

**One DO-NOT-SHIP-without gate:** the code change itself is safe to build and dry-run test, but do
not put it on the owner's car until at least one LIVE bench flash has run with a host forced to
the old 30 s idle timeout. Details in "Verification limits" below.

---

## 1. The goal

A WiCAN running this firmware must survive an ECU erase of any realistic length when the PC tool
is NC Flash **2.12.0 or older** (host idle timeout 30 s), without the PC tool changing.
Secondarily: once an erase may have started, the firmware must never abandon the ECU because of
anything the *host* did — socket close, WiFi dropout, host crash — because the firmware alone can
finish the flash and leave the car driveable.

Host facts this rests on (all verified in nc-flash-126):

- 2.12.0 idle timeout: `_FAST_WRITE_IDLE_MS = 30000` (`src/ecu/wican_transport.py:189` at
  575b3c0~1). 2.13.0: 90000 (line 201 at HEAD).
- The idle clock resets on **any received bytes**, even a partial line:
  `buf.extend(chunk); last_data = time.monotonic()` runs before any parsing
  (wican_transport.py:826-827). So any heartbeat bytes keep every host version alive.
- The parse loop is **byte-identical in every host version that has fast_write** — introduced at
  43aa58e, unchanged since except the idle constant (verified by diff of 43aa58e vs 575b3c0~1 vs
  HEAD). Hosts older than 43aa58e have no fast_write command at all and cannot reach this path. So
  there is exactly ONE parser to satisfy.
- That parser silently ignores any line that does not start with `FWERR`, `NCFWDONE`, or
  `NCFWPROG` (wican_transport.py:843-859), and a malformed `NCFWPROG` is swallowed by
  `except (IndexError, ValueError): pass`. A repeated `NCFWPROG done/total` just re-calls
  progress_cb with the same numbers.
- The host is an **observer only**: "the firmware owns the flash, so killing the socket does NOT
  stop it (there is deliberately no host-side abort)" (fast_write docstring,
  wican_transport.py:773-776). The staged image, manifest and CRC are all on the SD card, so the
  firmware can finish a flash with no host at all. This is what makes "keep going when the host
  dies" not just safe but the designed-for recovery.
- One ceiling the heartbeat does NOT extend: the host's overall `FAST_WRITE_TIMEOUT_MS = 600000`
  (wican_transport.py:185) is an absolute wall-clock deadline, not reset by bytes. Total flash
  time (transfer + up to 60 s per edge + up to 60 s TransferExit) must stay under 10 minutes.
  Today's worst case is roughly 2–4 min of transfer + 180 s of ceilings — comfortable, but do not
  grow the ceilings later without re-checking this.

## 2. The design

### 2.1 Where the heartbeat lives

The stall happens inside `recv_matching()` (ncflash_fastwrite.c:230-241), reached from two places
sharing one edge deadline: the Flow-Control wait (`fw_isotp_send`, :284) and the ACK wait
(`fw_await_positive`, :317). Neither has the tx_queue.

**Chosen mechanism: a file-scope keepalive context + slicing inside `recv_matching()`.** A small
static struct (this file already uses exactly this pattern for `s_fw_err_stage`/`s_fw_err_nrc`,
:100-104, with the comment "without threading them through every goto site"):

```c
static struct {
    bool armed;
    QueueHandle_t *tx_queue;
    int64_t next_beat_us;
    uint32_t done, total;    /* numbers for the NCFWPROG line */
    uint32_t dropped;        /* beats that could not be queued */
} s_ka;
```

- `fw_ka_arm(tx_queue, done, total)` sets `next_beat_us = now + KEEPALIVE_MS*1000`;
  `fw_ka_disarm()` clears `armed`.
- `recv_matching()` changes only when armed: cap each `can_receive()` wait at `min(ms to overall
  deadline, ms to next_beat_us)`; at the top of each loop pass call `fw_ka_tick()`, which — if
  `now >= next_beat_us` — formats `NCFWPROG done/total\n` and try-sends it with a **0-tick**
  `xQueueSend`, then sets `next_beat_us = now + interval` (from *now*, so a late beat never bursts
  to catch up). On queue-full it just increments `dropped`. When `armed == false`,
  `recv_matching()` must compute exactly what it computes today — the non-edge waits are provably
  untouched.
- Arm/disarm sites: around the edge-block `fw_transfer_data()` call (arm just before :585's
  `edge_t0`, disarm after :588 returns), and **also around `fw_transfer_exit()`** (:360-367) — it
  carries the same 60 s ceiling, which is already above the old host's 30 s idle clock; without a
  heartbeat there, a slow finalise makes a 2.12.0 host declare failure on a flash that succeeded
  and push the user into a needless re-flash. Same mechanism, third arm site.

This is safe against frame loss: the TWAI driver buffers up to 96-100 RX frames (`rx_queue_len`,
can.c:123,446-447), the emit between slices costs microseconds (0-tick send), and both the edge
deadline and the beat schedule are absolute `esp_timer` values, so slicing cannot drift any
timeout. A frame that arrives exactly at a slice boundary sits in the driver queue and is returned
by the very next `can_receive()` call.

**Why not the alternatives:**

- *Callback parameter threaded through `fw_isotp_send`/`fw_await_positive`/`fw_transfer_data`*:
  same behavior, but changes four signatures and every call site (including the six non-edge
  callers that must pass "no callback"), for zero functional gain. More edits in ECU-touching code
  = more places to slip. The static context is one struct, three arm/disarm sites, one function
  touched.
- *Hoisting the wait loop into the caller*: would mean re-implementing the FC-then-ACK sequencing
  and the 0x78-pending ride-out (:320-323) at the top level — a rewrite of working, bench-proven
  protocol code. Highest risk option, rejected.
- Concurrency note: everything here runs on one task (`can_tx_task`, main.c:279-308; guarded
  single-op by `s_fwbusy`, :408-413), so the static context needs no locking.

### 2.2 THE CRUX — heartbeat failure mid-erase

**Ruling: a heartbeat that cannot be queued is IGNORED. It never sets host_gone, never aborts, and
never blocks.** Concretely, the heartbeat never calls the 2-second `tx_send()` at all — it uses a
0-tick try-send. So the failure mode "we detect the dead host at second 10 of the erase and
abandon a mid-erase ECU" cannot be built, and neither can "2 s blocking sends eat the erase
budget": a failed beat costs microseconds.

There is not even a real need for "stop after the first failure": with `can_rx_task` suspended and
both queues drained at op start (:449-454), the flash is the only producer into a 32-deep queue
(main.c:797). Even with the host stone dead, the first 32 beats (~160 s at 5 s) enqueue
successfully and later ones fail instantly and are counted. No retry storm is possible.

**Can the flash usefully finish with the host gone? Yes — and it must.** The host is an observer
(docstring cited above); everything needed is on SD; the alternative is an ECU with no
application. "Nobody is watching the progress" is answered by the event log: `EVL_FLASH_OK` /
`EVL_FLASH_FAIL` already record the outcome (:649, :679), and the design adds one `EVL_INFO` line
when `dropped > 0` so a post-mortem says plainly "the PC tool stopped listening at block N; the
flash finished anyway".

### 2.3 The point of no return — the generalisation

The heartbeat alone is not enough, because of a hole that exists TODAY with matched versions:
after the erase, every 16th block emits `NCFWPROG` through the 2-second `tx_send`, and a failure
there does `host_gone = 1; rc = -3; goto cleanup;` (:628) — abandoning a half-written, freshly
erased ECU because WiFi hiccuped for 2 s. The same is true of the pre-edge emit at :582 when it
runs for the *region-1* edge (by then the SBL has been fully transferred and the ECU may already
be erasing — the comment at :578 "aborting HERE is pre-erase" is only true for the first edge).

**Rule: define `s_ponr` (point of no return), set immediately BEFORE sending the last SBL block**
(the `r == 0 && rem == take` block, :557 — chosen because the comment at :553-556 says the ECU may
start erasing on that block's ACK or on the next one; taking PONR any later would leave a window
where an abort abandons an erasing ECU). From `s_ponr = 1` until cleanup:

- Every `fw_emit`/`fw_emit_err` send becomes a **0-tick try-send**; a failure increments `dropped`
  and the code carries on. No post-PONR path may set `host_gone` or `goto cleanup` because of a
  queue/socket problem. This covers the per-16-blocks progress line (:628), the region-1 pre-edge
  line (:582), `NCFWDONE` (:646), and a terminal `FWERR` (:590, :639) — the FWERR case still
  aborts, but because the *ECU* failed, with the line delivered best-effort exactly as its comment
  already promises (:147).
- Behavior BEFORE PONR is unchanged: 2-second bounded sends, and an emit failure still aborts with
  rc=-3 — that is correct there, because nothing on the ECU has been erased (SBL blocks are
  delivered into RAM; the ECU's application is intact until the SBL runs) and aborting pre-erase
  is the safe direction, exactly as the existing :575-578 comment argues.
- Cleanup: extend the existing host-gone queue drain (:665-670) to also run when `dropped > 0`, so
  ≤32 stale progress lines never spew at the next client that connects.
- Outcome reporting: a flash that completes with the host gone returns rc=0 and logs
  `EVL_FLASH_OK`; the new `EVL_INFO dropped` line marks the lost host. `rc=-3 host_gone` becomes a
  strictly pre-erase verdict — which also makes the `reading-a-flash-fail` reading simpler, not
  harder.

Why 0-tick and not a short bounded send post-PONR: queue-full with the drain task alive means the
TCP socket has not accepted bytes for ≥32 lines — the host is effectively unreachable and the
bytes would not arrive anyway; blocking longer buys nothing and costs ECU time inside armed waits.
The 32-deep queue itself is the tolerance for transient WiFi jitter.

### 2.4 The edge budget

**The deadline stays measured exactly as today: `edge_t0` taken immediately before
`fw_transfer_data()` (:585-587), after the pre-edge emit.** Nothing needs compensating, because
the design removed every blocking send from inside the wait: beats are 0-tick, and slice
arithmetic uses the absolute `deadline_us` that `fw_ms_left()` already re-derives each pass
(:250-255), so the ECU keeps its full 60 s of genuine listening time. Rule to state in code: **no
code inside an armed wait may block on anything except `can_receive()`**.

### 2.5 The interval

**KEEPALIVE_MS = 5000.** Reasons: the binding constraint is the 2.12.0 host's 30 s; 5 s gives 6×
margin, meaning five consecutive beats can be lost or delayed before the host fires — covering
host GC/UI stalls, the host's 1 s select granularity, TCP delayed-ACK/Nagle (the slcan port does
not set TCP_NODELAY — slcan_port.c has none — so a lone 20-byte line can wait ~an RTT or a
delayed-ACK interval; that is milliseconds against a 5 s beat, but the implementer may add
TCP_NODELAY on the port socket as optional hardening), and WiFi retry bursts of a few seconds.
Going much lower buys nothing and adds noise; going near 10-15 s starts spending the margin that
exists to absorb exactly the hiccups we cannot schedule. Add
`_Static_assert(3 * KEEPALIVE_MS < 30000, ...)` naming the 2.12.0 constant so nobody can later
drift it past the old host.

**Do the two 60 s edges still need their own treatment? No.** With a heartbeat, the host's idle
clock never fires no matter how the ECU splits its erase, so the 60+60 host race that #126's
one-shot emit was bounding is gone entirely. `RESP_ERASE_TIMEOUT_MS` becomes what it should be: a
pure "is the ECU dead" ceiling, sizeable on ECU evidence alone (12.6 s measured, 60 s = 4.7×
headroom), decoupled from every host version. Keep it at 60000 in this change; the :65-69 comment
block ("MUST stay comfortably under the host fast-write idle timer... Change together") must be
rewritten to say the coupling is now the heartbeat, and the only remaining host-side wall is the
absolute 600 s total (see §1 last bullet).

### 2.6 The line to send

**Reuse `NCFWPROG <done>/<total>\n` with the current block numbers. No new marker word.** Grounds:
the single deployed parser ignores unknown lines, so a new word would not crash anything — but it
also buys nothing, since 2.13.0 needs no discrimination (its clock resets on bytes too), while
`NCFWPROG` additionally re-feeds progress_cb so the host UI visibly stays alive during the stall
instead of freezing. Constraints the line already satisfies and must keep: it must not begin with
`FWERR` or `NCFWDONE` (those prefixes are terminal at the host, :843-850 — this is the one way a
keepalive could actively *kill* a host session, so state it in code as a comment), and it should
end in `\n` so the host consumes it cleanly (though even a partial line resets the clock).
Repeated identical `done/total` values are established as harmless by the already-shipped one-shot
edge emit (#126, :559-583).

### 2.7 Watchdogs, sleep, leases

- **Task WDT: no risk, before or after.** `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5` watches only the idle
  tasks (sdkconfig:1706-1711); nothing in main/ calls `esp_task_wdt_add`, so `can_tx_task` is not
  subscribed. A task *blocked* in `twai_receive` yields the CPU, the idle task runs, the WDT is
  fed — a 60 s blocked wait cannot trip it today, and slicing (still blocked ≥99.9% of the time)
  changes nothing. The `vTaskDelay(1)` at :630 exists for the CPU-bound streaming loop (dry-run),
  not for these waits; leave it alone.
- **Sleep interlock: holds.** Sleep gates on `busy_flashing = can_flash_active() || ...`
  (sleep_mode.c:1055), and `FLASH_ACTIVE_BIT` is held across the whole op including silent edges
  (:416, :687). The stuck-flash ceiling is 30 minutes (sleep_mode.c:649) — two 60 s edges never
  approach it. A host-gone-but-continuing flash keeps `busy_flashing` true, so sleep stays
  postponed until the ECU is finished.
- **#92 leases: hold.** If the host dies mid-flash its bus-claim lease expires and the reaper
  clears it, but datalog auto-resume explicitly requires `!can_flash_active()` (main.c:392) and
  `can_should_park()` ORs the flash bit (can.c:254), so no producer can inject a frame into the
  UDS session while the firmware finishes alone. Producers un-park only at
  `can_flash_active_clear()` in cleanup — after the ECU is complete or the op is over either way.

### 2.8 Other brick paths found

1. **The post-erase progress-abort hole (§2.3) is the biggest one** — it bricks on a 2 s WiFi
   stall with *matched, current* versions, at any block after the erase. WiFi hiccups are far more
   common than >30 s erases. The PONR rule closes it. If for some reason the PONR generalisation
   is not wanted, ship it anyway for the emits between PONR and flash end — shipping only the edge
   heartbeat would leave the commonest brick path open.
2. **Half-open socket** (laptop lid closed, WiFi vanished): firmware `send()` succeeds into its
   own TCP buffer until the port's TCP keepalive (5 s idle/5 s interval/3 probes,
   slcan_port.c:46-48) kills the connection ~15-20 s later, `PORT_OPEN` clears, the drain task
   parks, the queue fills, beats start dropping at 0 cost. Post-PONR: flash finishes. Correct by
   construction under this design; worth stating because it is the *slow* version of host death
   and the one a bench test with `kill` does not exercise.
3. **Host reconnect mid-flash**: a new client on 35001 starts draining the same queue and may
   receive stale `NCFWPROG` lines. Pre-existing behavior, not worsened; `version_ping` scans only
   for `NCFRv` and ignores them. No action, but noted.
4. **Host absolute 600 s deadline**: a pathologically slow ECU (max 0x78-pendings, slow STmin)
   could push past it; the host errors and closes, and post-PONR the firmware still finishes the
   ECU. The host then shows "failed" for a flash that succeeded — acceptable, log line explains
   it.
5. **`fw_transfer_exit` silent-finalise vs old hosts** — covered by the third arm site (§2.1);
   without it a >30 s finalise makes 2.12.0 report failure on a good flash.

## 3. Constraints — what must NOT change

- No new wire marker words: `grep -o "NCFW[A-Z]*"` over ncflash_fastwrite.c must show only
  NCFWSYNC / NCFWPROG / NCFWDONE (plus the NCFWv1 header constant). FWERR format unchanged.
- `RESP_ERASE_TIMEOUT_MS` stays 60000; `FC_TIMEOUT_MS`, `RESP_FIRST_TIMEOUT_MS`,
  `RESP_PENDING_TIMEOUT_MS`, `MAX_PENDING`, `TX_QUEUE_SEND_TIMEOUT_MS` unchanged.
- `recv_matching()` must be bit-for-bit equivalent to today's behavior when the keepalive is not
  armed (all non-edge waits, and the entire fast-READ path if it shares helpers — it does not, but
  check).
- Pre-PONR emit failures keep today's abort (rc=-3 host_gone). `NCFWSYNC` and all pre-flash gate
  errors (stages 1-8) unchanged.
- Ownership and ordering unchanged: `can_rx_task` suspend/drain at entry, `FLASH_ACTIVE_BIT`
  set-first/clear-last, cleanup order (:654-689).
- Dry-run ('D') behavior unchanged (edges are inside `if (live)`, so the heartbeat never arms in
  dry-run — do not "fix" that).
- Nothing inside an armed wait may block except `can_receive()`.
- No changes to the host repo, sleep_mode.c, can.c, or slcan_port.c in this change (TCP_NODELAY is
  optional hardening for a separate commit if wanted).

## 4. Acceptance criteria (each transcript-verifiable)

1. **Build**: `idf.py build` (per the wican-build skill) exits 0; transcript shows the success
   line.
2. **Interval + guard**: `grep -n "KEEPALIVE_MS" main/ncflash_fastwrite.c` shows the constant
   defined as 5000 and a `_Static_assert` tying `3 * KEEPALIVE_MS < 30000` with a comment naming
   the 2.12.0 host constant.
3. **Three arm sites**: grep shows `fw_ka_arm` (or equivalent) called exactly three times: at the
   erase-edge block (one shared site covering both edges) and around `fw_transfer_exit`; and a
   matching disarm on every path out (including error paths — show the disarm placement in the
   transcript diff).
4. **Slicing**: grep of `recv_matching` shows the beat-capped wait and a tick call, and shows the
   un-armed path computes the same remaining-time expression as before (transcript quotes the
   function).
5. **0-tick beats**: grep shows the keepalive send uses `xQueueSend(..., 0)` (or a `tx_try_send`
   wrapper with 0 ticks) and that no `pdMS_TO_TICKS(TX_QUEUE_SEND_TIMEOUT_MS)` send is reachable
   from inside an armed wait.
6. **PONR rule**: grep shows `s_ponr` set before the last-SBL-block send, and shows that every
   fw_emit failure site after it (edge pre-emit, per-16 progress, NCFWDONE, FWERR) no longer sets
   `host_gone`/`goto cleanup` post-PONR; the pre-PONR sites still do.
7. **No new markers**: `grep -o "NCFW[A-Z]*" main/ncflash_fastwrite.c | sort -u` output shown,
   containing only NCFWDONE NCFWPROG NCFWSYNC (header constant aside).
8. **Constants untouched**: grep shows RESP_ERASE_TIMEOUT_MS 60000 and the other timeout defines
   unchanged.
9. **Dry-run regression on the bench device**: a mode-'D' fast write against the bench WiCAN
   completes with `NCFWDONE` streamed to the host side (transcript shows the command and the
   DONE), proving the touched file still runs the full non-live path.
10. **Comment debt**: the :65-69 "Change together" comment block is rewritten to describe the
    heartbeat as the host-side coupling (grep shows the new wording).

## 5. Verification limits and the manual checklist (outside `/goal`)

**What cannot be verified from a transcript:** the armed-wait code path itself only runs inside
`if (live)` during a real erase. Code reading, the build, and the dry-run prove everything
*around* it; they do not prove a beat is actually emitted mid-erase. A temporary forced-stall
build would test modified code, not the shipping code — recommended against as the confidence
source.

**Safest way to gain real confidence, in order:**

1. This document's adversarial review + the implementer's diff walk-through (free, no hardware).
2. **The gold test, owner's call:** the bench measured a real 12.6 s erase on a live PCM on
   2026-08-23, so a live bench flash exists as a procedure. Repeat it with the host patched to
   `idle_ms=30000` (one-line local edit simulating 2.12.0). If the flash completes with the
   progress bar ticking through the erase, the whole feature is proven end-to-end. Risk to accept:
   a failed live flash leaves the bench PCM without an application until re-flashed with the same
   tool (recoverable, but it is a real ECU erase).
3. Second live bench flash: `kill` the host process at ~50%, after the erase. Expect: firmware
   finishes, `EVL_FLASH_OK` plus the new dropped-lines `EVL_INFO` in `/load_event_log`, ECU alive.
   This proves the PONR rule.
4. Only after 2 and 3 pass: the owner's car.

**Ship ruling:** no part of the design is DO-NOT-SHIP as code, but **DO-NOT-SHIP to the owner's
car until manual items 2 and 3 have passed on the bench** (this is also what the
bench-test-before-merge rule already requires). What would change that: if no live-flashable bench
PCM is available any more, then merge only with the owner's explicit acceptance that the
armed-wait path ships code-read-only, stated in the PR.

Manual checklist:

- [ ] Live bench flash, host forced to idle_ms=30000: completes, no "fast write stalled", progress
      bar moves during the erase.
- [ ] Live bench flash, host killed post-erase: firmware finishes alone; event log shows FLASH_OK
      + dropped-lines INFO; PCM boots.
- [ ] Optional: live flash with 2.13.0 unmodified — confirms no regression for the matched pair.
- [ ] Confirm bench sleep config still on test values afterwards, per the standing bench-config
      note.

## 5b. As built — where the code differs from this design

Three deltas, all from the adversarial review of the first cut. The design above is otherwise
implemented as written.

1. **The cleanup drain runs on `host_gone` alone**, not on `host_gone || dropped > 0` as §2.3 says.
   Draining on `dropped` would swallow the `NCFWDONE` (or the `FWERR`) that a host which stalled
   and then recovered is still waiting for — reporting failure for a flash that worked, which is
   the same needless re-flash of a healthy ECU that §2.8.5 exists to prevent. Stale lines left for
   a genuinely dead host cost nothing: the next op drains the queue before it starts, and
   `version_ping` ignores lines it does not recognise.
2. **The point of no return is keyed on `erase_edge`**, not on `r == 0 && rem == take` as §2.3
   says. A manifest declaring `sbl_len = 0` skips region 0 entirely — the block-size gate allows
   it — so the narrower test would never fire, yet region 1's first block still armed the
   keepalive, leaving every post-edge emit a blocking send that aborts on failure. Keying on
   `erase_edge` is behaviour-identical for real manifests and makes "armed implies past the point
   of no return" true at all three arm sites. No assert guards that invariant: a panic mid-flash
   is itself a brick path.
3. **The lost-host event line is worded by outcome and reports the first drop.** `done` at cleanup
   equals `total_blocks` on success, so the design's wording would always have read "block N/N",
   and "the flash carried on without it" is false when the ECU is what failed. A `drop_blk` field
   records the block at the first undelivered line, and `rc == 0` picks the wording.

## 6. Ready-to-paste `/goal` line

```
/goal Implement docs/goals/erase-edge-keepalive.md: idf.py build exits 0; grep evidence shows KEEPALIVE_MS=5000 with the 3x<30s static assert, keepalive armed at both erase edges and around fw_transfer_exit with disarm on all exits, recv_matching slicing that is unchanged when unarmed, 0-tick keepalive sends with no blocking send reachable inside an armed wait, s_ponr set before the last SBL block with no post-PONR emit failure aborting the flash, wire markers limited to NCFWSYNC/NCFWPROG/NCFWDONE, all timeout constants unchanged, the "Change together" comment rewritten; and a bench dry-run ('D') fast write returns NCFWDONE. (Live-flash items are the manual checklist, outside this condition.)
```
