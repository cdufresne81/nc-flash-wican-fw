/*
 * This file is part of the WiCAN project.
 *
 * Copyright (C) 2022  Meatpi Electronics.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * Native-TWAI request/response datalogger -- Phase B, "measure-first".
 *
 * Why this exists: the AutoPID poll loop is ~0.6 Hz/channel because it is one serial
 * ELM327-emulation loop with a HARDCODED vTaskDelay(100ms) between every PID request
 * (autopid.c:4190) plus a ~1.7 s ATMA broadcast window per cycle. That 100 ms floor is a
 * firmware artifact, NOT an ECU limit: a Denso PCM answers a single-frame mode-01/22 PID
 * in ~5-8 ms (request airtime + ECU turnaround). This module polls those PIDs straight over
 * the native TWAI controller (can_send -> twai_transmit for the request, can_receive for the
 * 0x7E8 reply) with NO artificial delay, single-PID round-robin, and logs the real per-request
 * turnaround so we can size batching (mode-01 multi-PID / UDS multi-DID) against measured data
 * instead of an estimate.
 *
 * Difference from fast_log (Phase A): fast_log is LISTEN_ONLY (passive broadcast). poll_log is
 * NORMAL / on-bus -- it TRANSMITS diagnostic read requests (the same non-intrusive reads every
 * scan tool issues). It is a sibling mode; broadcast sniffing is unchanged and still available.
 *
 * Pure poll-only: this mode does NOT decode broadcast frames (it discards them while waiting for
 * the response). The broadcast channels (RPM/VSS/ECT...) therefore stay empty in the CSV here --
 * that is intentional, so the experiment isolates the polled-channel rate. The eventual production
 * mode is the hybrid (passive broadcast + polling the rest).
 *
 * Single-consumer discipline: in POLL_LOG mode this task is the SOLE twai_receive()/twai_transmit()
 * caller. can_rx_task() is gated off for POLL_LOG in main.c so it never steals response frames
 * (TWAI delivers each frame to exactly one waiter). All bus access goes through can.c so the
 * CAN_ENABLE_BIT / can_block() teardown fence quiesces us during an OTA-upload or sleep can_disable().
 *
 * Teardown safety: every can_receive() uses timeout 0 (non-blocking) and every can_send() uses
 * timeout 0, exactly like fast_log. A blocking receive/transmit could be parked inside the driver
 * when an OTA/sleep can_disable() runs can_block() (clears CAN_ENABLE_BIT, ~1 ms, then
 * twai_driver_uninstall()) -> PANIC. Timeout 0 returns at once so the next can.c call parks on
 * CAN_ENABLE_BIT instead of inside the driver.
 */

#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <math.h>
#include <float.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "driver/twai.h"

#include "can.h"
#include "autopid.h"
#include "csv_logger.h"
#include "event_log.h"
#include "config_server.h"
#include "expression_parser.h"
/* Battery voltage for the recording gate (fast_log already REQUIRES main). NOTE: sleep_mode.h
 * declares sleep_mode_get_voltage() TWICE behind #if HARDWARE_VER -- esp_err_t on WICAN_PRO, but
 * int8_t (1 on success) on the older targets. The `== ESP_OK` test below is right for the PRO
 * build this product ships; it would need revisiting if an older target is ever revived. */
#include "sleep_mode.h"
#include "vehicle.h"        /* VEHICLE_IGN_HYSTERESIS_V -- shared with the CSV ignition gate */

#include "poll_log.h"

static const char *TAG = "poll_log";

/* ---- Tunables ----------------------------------------------------------- */
#define POLLLOG_TX_ID            0x7E0u  /* physical request to the PCM (works for mode-01, -22 & -23) */
#define POLLLOG_RX_ID            0x7E8u  /* PCM positive-response ID */
#define POLLLOG_PAD              0x55u   /* ISO-TP single-frame padding (ECU ignores unused bytes) */
#define POLLLOG_RESP_TIMEOUT_MS  30      /* per-PID wait budget for the response */

/* Mode 0x23 ReadMemoryByAddress (issue #51). The request is SID + 4-byte big-endian
 * address + 2-byte big-endian size = 7 payload bytes, which is what sets the request
 * cap below; modes 01/22 need only 1..3. Bench-verified 2026-07-25 against the patched
 * NC PCM: this ALFID-less shape is the ONLY one it accepts (every ISO-14229 ALFID
 * variant and every 3-byte-address form answers NRC 0x12/0x31), matching the wire spec
 * ncflash_fastread.c:159-165 already proved for bulk ROM reads. A STOCK ROM refuses the
 * service outright with NRC 0x22 at dispatch, so mode 23 needs the patched calibration.
 *
 * The positive response is 63 <data...> with NO address/size echo, so the data begins at
 * frame byte B2 (mode 01 = B3, mode 22 = B4) -- see polllog_match. Payload is 1 SID byte
 * + size, so a size of 1..6 stays in one ISO-TP single frame and 7+ would need
 * FF/FC/CF reassembly this path deliberately does not implement (bench-confirmed
 * boundary; the real speeps channels are 1, 2 and 4 bytes). Oversized or malformed
 * mode-23 rows are rejected in polllog_req_bytes, which is the single pollability
 * funnel, so they never reach the sweep or the schedule. */
#define POLLLOG_SVC_RMBA         0x23u   /* UDS ReadMemoryByAddress service (== UDS_RMBA, main/ncflash_fastread.c) */
#define POLLLOG_RMBA_MAX_SIZE    6       /* bytes readable in one single frame (7 - the 0x63 SID) */
#define POLLLOG_RMBA_REQ_BYTES   7       /* a mode-23 request is EXACTLY this: SID + addr4 + size2 */
/* Longest request any supported service frames. Equal to the mode-23 length today, but a
 * DIFFERENT fact: keeping them separate means adding a longer service later widens the buffer
 * without silently loosening the mode-23 shape gate below. */
#define POLLLOG_MAX_REQ_BYTES    POLLLOG_RMBA_REQ_BYTES

/* Requested read size of a mode-23 request, from the operand poll_log framed. One definition,
 * so the byte offsets of the size field are not restated in polllog_match(). Valid only once
 * polllog_req_bytes() has accepted the request (it enforces the exact length). */
static inline uint16_t polllog_rmba_size(const uint8_t *req)
{
    return (uint16_t)(((uint16_t)req[5] << 8) | req[6]);
}
#define POLLLOG_RX_TASK_PRIO     5       /* == can_rx_task; sole TWAI consumer in POLL_LOG */
#define POLLLOG_RX_STACK_BYTES   (1024 * 8)   /* INTERNAL RAM (brick invariant) */
#define POLLLOG_STATS_PERIOD_US  (3LL * 1000 * 1000)    /* emit turnaround stats every 3 s */
#define POLLLOG_GUARD_STABLE_US  (15LL * 1000 * 1000)   /* clear crash-guard after 15 s stable */

/* ---- Engine-off LISTEN_ONLY quiesce (Stage 1) -------------------------- */
/* When the ECU stops answering (engine/key off), stop transmitting and flip the bus to
 * LISTEN_ONLY so the dongle no longer keeps the vehicle CAN bus awake; resume on the first
 * received frame (the car woke). All thresholds are CAN-derived -- no voltage dependency, so a
 * parked battery sitting above sleep_volt can never trip or block it. */
#define POLLLOG_ENGINE_OFF_MS    5000     /* confirmed-running: no OK this long -> engine OFF (quiesce) */
#define POLLLOG_PROBE_MS         2000     /* probing (no OK yet, boot or resume): re-quiesce after this -> kills the dead-bus spin */
#define POLLLOG_FLIP_MIN_MS      2000     /* min dwell between mode flips (anti-thrash hysteresis) */
#define POLLLOG_MAX_QUIESCE_MS   600000   /* 10 min: force NORMAL+repoll even with no frame (self-heal) */
#define POLLLOG_RESUME_FRAMES    1        /* an RX frame triggers a PROBE-resume; confirmed only by a real OK */
#define POLLLOG_FLASH_PARK_MS    20       /* interlock park sleep while a flash owns the bus (task #36) */

/* Divisor-gate staleness bypass (issue #29). Half the engine-off budget. If no OK has
 * landed for this long, poll EVERYTHING regardless of divisors, so a long divisor on the
 * only answering channel can never let (now - s_last_ok_us) reach POLLLOG_ENGINE_OFF_MS
 * and fire a FALSE IGNITION_OFF. Costs nothing on a healthy config (an OK lands every
 * min_n sweeps, i.e. tens of ms) and does not weaken the real detector: with the engine
 * genuinely off, un-gated full sweeps still yield no OK and we still quiesce at 5 s. */
#define POLLLOG_GATE_STALE_MS    (POLLLOG_ENGINE_OFF_MS / 2)

/* ---- Recording gate: WATCH vs FAST ------------------------------------- */
/* THREE facts here read almost alike, so name them precisely (#98):
 *   s_bus_normal     -- we are NOT quiesced, i.e. transmitting. Goes true on ANY bus frame, so it
 *                       does NOT mean the ECU replied. Exact complement of s_quiesced.
 *   s_ecu_answering  -- the ECU replied to a request WE sent, this session. This is the real
 *                       "the ignition is on" evidence, and what poll_log_ignition_on() reports.
 *   s_gate_open      -- the RECORDING gate: voltage + RPM say the ENGINE is actually turning.
 *
 * An answering ECU still does not mean the engine turns -- a car sitting at key-on with the engine
 * off answers every request, so the sweep used to run at the full ~430 req/s whenever the key was
 * on and whether or not anything was being recorded. That is ECU diag-task load, bus utilisation
 * and power spent on samples nobody keeps.
 *
 * WATCH is the same full sweep held to one pass per POLLLOG_WATCH_SWEEP_MS (~16-19 req/s on a
 * 19-PID table). Deliberately the SAME sweep and not an RPM-only probe: every channel stays warm,
 * and a table with no RPM row still works -- the gate simply drops the RPM term. FAST is today's
 * path unchanged.
 *
 * PROBE is never slowed (see the pacing block): resume-in-one-frame depends on it.
 *
 * The gate cannot react faster than the battery ADC, which refreshes every 3 s
 * (main/sleep_mode.c), so a faster watch cadence would buy nothing. */
#define POLLLOG_WATCH_SWEEP_MS   1000     /* one full sweep per second while waiting for the engine */
#define POLLLOG_WATCH_CHUNK_MS   20       /* the inter-sweep wait is chunked at this step -- see below */
/* Both borrowed, not re-typed: the CSV writer closes a trip on the same debounce and vehicle.c
 * uses the same band, so a copy here could silently drift and leave the poller and the writer
 * disagreeing about when the engine stopped. */
#define POLLLOG_GATE_OFF_MS      CSV_LOGGER_IGN_OFF_DEBOUNCE_MS
#define POLLLOG_GATE_HYST_V      VEHICLE_IGN_HYSTERESIS_V
#define POLLLOG_GATE_RPM_ON      400.0f   /* under any idle, over cranking noise */
#define POLLLOG_GATE_VOLT_DEF    VEHICLE_ENGINE_ON_VOLT_DEFAULT  /* fallback when engine_volt is unreadable (vehicle.h) */
#define POLLLOG_RPM_STALE_MS     (2u * POLLLOG_WATCH_SWEEP_MS)  /* older than this and RPM stops counting */

/* ---- ENGINE_ON / ENGINE_OFF event lines -------------------------------------------------
 * WHY THIS EXISTS. On 2026-08-17 the owner ran an engine-on/off cycle in the car to verify a fix,
 * and the event log afterwards was read as "the engine never ran" -- every voltage line sat at
 * 12.75-12.86 V and no event anywhere said otherwise. That reading was WRONG: a CSV trip only opens
 * while the engine is running, so the DATALOG_OPEN line WAS the proof. The fact was present but
 * implicit, and a verification test got misread because of it. There is also an asymmetry to fix:
 * IGNITION_ON and IGNITION_OFF both exist, but the crank turning -- which the gate below ALREADY
 * computes, every single sweep -- was never announced.
 *
 * So these lines carry no new detection. They are the edge of the gate's OWN rpm state
 * (rpm_known / rpm_running in polllog_eval_gate), announced once. Deliberately not a second,
 * independent "engine on" notion: two detectors WILL drift apart and then the log and the recording
 * gate disagree, which is worse than no line at all.
 *
 * EDGE-TRIGGERED, latched in s_engine_on. polllog_eval_gate runs once per sweep -- up to 100 times
 * a second in FAST -- so an unlatched line would write the SD card full in a minute. Same
 * one-line-per-episode rule the sleep-mode veto/postpone latches follow (main/sleep_mode.c:1530).
 *
 * THRESHOLDS AND HYSTERESIS. Starting is announced on the FIRST sweep that sees rpm over
 * POLLLOG_GATE_RPM_ON (400) -- the same threshold the gate opens on, chosen to sit under any idle
 * and over cranking. It is NOT confirm-counted, and that is a deliberate trade, not an oversight:
 * the whole point of the line is to land BEFORE the DATALOG_OPEN it causes, and the gate opens on
 * that same first sweep. Even one extra pass of confirmation would, at watch cadence, put this line
 * a second late -- i.e. after the CSV writer has already noticed the gate and opened the trip.
 *
 * All of the hysteresis therefore lives on the STOP edge: stopping needs rpm confirmed under the
 * threshold and held there for POLLLOG_GATE_OFF_MS (3 s, the same debounce that closes the gate and
 * the trip). That alone is what bounds a noisy channel -- an ON/OFF pair costs at minimum those
 * 3 s, so no burst is possible, and a momentary dip through zero never reaches the log at all.
 * A genuine stall-and-restart DOES produce a pair, which is correct and worth seeing.
 *
 * RPM 0 BECAUSE NOTHING ANSWERED YET IS NOT "ENGINE OFF". rpm_known is false until a real value
 * lands (s_rpm_seen) and goes false again once one is older than POLLLOG_RPM_STALE_MS, and only a
 * KNOWN-low reading may arm the stop debounce. Combined with the latch -- ENGINE_OFF is only ever
 * emitted after an ENGINE_ON -- a boot with the key off, a table with no RPM row, and a probe that
 * never gets an answer all emit exactly nothing.
 *
 * ENGINE_OFF IS KEPT, not dropped for brevity. Two reasons beyond symmetry with IGNITION_OFF:
 * without it the latch would need clearing somewhere anyway (a latch left true after a drive means
 * the NEXT start is silent -- the exact failure this feature exists to prevent), and the run
 * duration it carries is the one number that makes a pair of lines readable at a glance.
 * DATALOG_CLOSE is not a substitute: it only exists if a trip happened to be recording. */
#define POLLLOG_ENGINE_OFF_CONFIRM_MS  POLLLOG_GATE_OFF_MS

/* The engine-off detector is wall-clock based, so it works unchanged at watch cadence: after
 * key-off every watch sweep is all-timeouts (~45 x 30 ms = ~1.4 s) and the 5 s budget still
 * fires. That only holds while a watch sweep stays comfortably inside the budget. */
_Static_assert(POLLLOG_WATCH_SWEEP_MS <= 2000,
               "watch cadence must stay well under POLLLOG_ENGINE_OFF_MS or engine-off detection slips");

/* HARD RATE CAP: no PID is ever polled more often than every POLLLOG_MIN_SWEEP_MS.
 * Each PID is requested once per sweep, so a floor on sweep duration IS a per-PID rate
 * cap: 10 ms -> 100 Hz, the shared product ceiling WICAN_LOG_MAX_HZ (config_server.h) that
 * also bounds the CSV grid and the broadcast throttle. Above that is overkill for this vehicle -- nothing on an NC
 * powertrain bus carries 100 Hz of real information -- and the cost is not free: it is
 * ECU diag-task load, bus utilisation, and CSV record rate, all spent on samples that
 * only duplicate their predecessor.
 *
 * It also makes the issue-#29 divisors predictable in wall-clock terms: with the floor
 * engaged, SampleEvery N means "at most every N x 10 ms", so N=4 is <= 25 Hz regardless
 * of how few PIDs are configured or how fast the ECU answers.
 *
 * The pacing delay is applied BEFORE the sweep-rate measurement, so sweep_ms/sweep_hz
 * (and therefore the Auto CSV grid) report the true achieved cadence and not the
 * un-paced rate the loop could have run at. The TWAI RX queue is not drained during the
 * delay: at ~2000 frame/s that is ~20 frames into a 96-slot queue, and the next sweep's
 * first polllog_poll_one drains stale frames before transmitting. */
#define POLLLOG_MIN_SWEEP_MS     WICAN_LOG_MIN_PERIOD_MS   /* shared 100 Hz ceiling (config_server.h) */
#define POLLLOG_MIN_SWEEP_US     ((int64_t)POLLLOG_MIN_SWEEP_MS * 1000)

/* GET /poll_status buffer. Was 400 (~285 chars typical); the issue-#29 schedule fields add
 * ~260 at max field widths. snprintf's return is CHECKED at the call site -- silent
 * truncation here emits invalid JSON to the web UI and to any polling tooling. */
#define POLLLOG_STATUS_JSON_SZ   1024

/* ---- Hybrid broadcast capture (ENABLED -- issue #7) ---------------------- */
/* Folds the passive broadcast decode into poll_log: every non-response frame poll_log already
 * drains while waiting (and previously threw away) is matched against the configured can_filters
 * and logged with source "CANFLT". Result: RPM/VSS/ECT/IAT/TPS/APP fill at bus rate ALONGSIDE
 * their polled copies, so "RPM [CANFLT]" and "RPM [PID]" sit side by side -- and it's ~free, the
 * frames are in hand. Runs inside the existing drain loop: no new task, no new TWAI consumer, so
 * the brick invariants are untouched. Enabled after the poll-only validation run confirmed polled
 * values on the vehicle (~325 req/s, 0 timeouts). Set to 0 to compile it back out entirely
 * (bit-identical poll-only behaviour) if a capture ever needs to isolate the polled path. */
#define POLLLOG_HYBRID           1
/* Per-broadcast-channel record throttle. Feeds the SAME wide CSV as the polled columns, so it
 * must not cap below the grid or broadcast columns staircase (repeat every other row) at a fast
 * grid. Unified onto the shared 100 Hz ceiling in #56 (was 20 ms / 50 Hz/ch). The 256-slot record
 * queue is the resource this guards; if 100 Hz/ch ever threatens it, grow the queue from
 * WICAN_LOG_MAX_HZ rather than throttling the rate back down. */
#define POLLLOG_BCAST_PERIOD_MS  WICAN_LOG_MIN_PERIOD_MS

/* ---- Independent one-shot RTC crash-guard ------------------------------- */
/* Distinct magic from fast_log (0xFA571A6D) and the CSV logger (0xA11C0DE5). If a prior boot
 * armed this and never cleared it (a crash during POLL_LOG bring-up), skip POLL_LOG for one boot
 * so a startup fault self-recovers instead of boot-looping. Recovery (safe-mode/AP/OTA) runs
 * upstream of poll_log_init() and is unaffected either way. */
#define POLLLOG_GUARD_ARMED      0x9011106Du
static RTC_NOINIT_ATTR uint32_t s_polllog_guard;

/* True for the rest of this uptime when the guard above made us skip bring-up. The skip is
 * one-shot ONLY if another boot follows it; since a wake-on-CAN now resumes in place instead of
 * rebooting, the sleep path reads this and takes the reboot fallback so the retry still happens.
 * See sleep_mode_recovery_needed(). */
static bool s_bringup_skipped = false;

static autopid_config_t *s_cfg = NULL;

/* Static task storage -> .bss -> INTERNAL RAM (no PSRAM on the hot poll path). */
static StaticTask_t s_rx_task_buf;
static StackType_t  s_rx_task_stack[POLLLOG_RX_STACK_BYTES];

/* Rolling turnaround stats, reset each time they are printed. */
static struct {
    int64_t sum_us;
    int64_t min_us;
    int64_t max_us;
    int      ok;
    int      timeout;
    int      txfail;
} s_st;

static void polllog_stats_reset(void)
{
    s_st.sum_us = 0;
    s_st.min_us = INT64_MAX;
    s_st.max_us = 0;
    s_st.ok = 0;
    s_st.timeout = 0;
    s_st.txfail = 0;
}

/* WiFi-visible snapshot, read by GET /poll_status via poll_log_get_status_json(). The cumulative
 * counters update LIVE on every poll (immediate answer/timeout feedback); the rtt/req-s metrics
 * refresh once per stats window. Plain aligned 32-bit fields -> atomic enough for a status
 * readout across the poll task and the httpd handler, so no mutex is needed. */
static volatile bool     s_active = false;
/* Live PID-table hot-swap (issue #39). s_reload_requested is set by the httpd handler
 * (poll_log_request_reload) and drained on THIS task at the top-of-loop safe point.
 * s_pid_count is a cross-task-safe mirror of s_cfg->pid_count so GET /poll_status never
 * dereferences the swappable s_cfg. s_last_reload_ok publishes the true outcome (the
 * handler answers optimistically before this task validates). */
static volatile bool     s_reload_requested = false;
static volatile bool     s_last_reload_ok = true;
static volatile uint32_t s_pid_count = 0;
static volatile uint32_t s_cum_ok = 0, s_cum_timeout = 0, s_cum_txfail = 0;
static volatile uint32_t s_win_ok = 0, s_win_timeout = 0, s_win_txfail = 0;
static volatile float    s_win_rtt_avg_ms = 0, s_win_rtt_min_ms = 0, s_win_rtt_max_ms = 0, s_win_req_s = 0;

/* Measured full-sweep rate (issue #23): wall time of one complete round-robin over all polled
 * PIDs plus the calculated-channel pass, EMA-smoothed (alpha 1/8). This is the fastest rate at
 * which every polled channel can deliver a FRESH value -- the "Auto" CSV grid tracks it via
 * poll_log_sweep_hz(). Only sweeps with >=1 OK are folded in, so probe sweeps against a silent
 * ECU (every PID timing out at 30 ms) can't poison the average; the value freezes at the last
 * good measurement across a quiesce and recovers within ~8 sweeps of a resume. Single-writer
 * (the poll task); 32-bit aligned floats -> atomic enough for lock-free readers, same contract
 * as the status snapshot above (deliberately NOT the 64-bit us value, which would tear). */
static volatile float s_sweep_hz = 0, s_sweep_ms = 0;

/* Per-PID sweep-divisor scheduling (issue #29). Single-writer (poll task) aligned scalars,
 * same lock-free contract as the status snapshot above. */
static volatile uint32_t s_sweep_seq    = 0;  /* one tick per gate pass (incl. empty sweeps); never reset */
static volatile uint32_t s_sweep_empty  = 0;  /* of those, how many requested nothing */
static volatile uint32_t s_sweep_pids   = 0;  /* PIDs actually requested in the last sweep */
static volatile uint32_t s_gate_skips   = 0;  /* cumulative polls suppressed by the divisor gate */
static volatile uint32_t s_pace_sweeps  = 0;  /* sweeps held back by the POLLLOG_MIN_SWEEP_MS cap */
static volatile uint32_t s_pids_gated   = 0;  /* pollable PIDs with sample_every >= 2 */
static volatile uint32_t s_pids_unpollable = 0; /* ENABLED rows the request funnel refused (issue #51) */
static volatile uint32_t s_sched_min_n  = 1;  /* min effective divisor over POLLABLE pids; 1 = nothing gated */
static volatile bool     s_gating_live  = false; /* the gate is actually in effect right now */
static volatile float    s_fast_ms = 0, s_fast_hz = 0;  /* fastest channel: sweep * sched_min_n */
/* Sweep-duration spread over the 3 s stats window -- the direct phasing-quality readout.
 * Working accumulators (poll task only) + published mirrors (read by the httpd task), same
 * two-stage contract as s_st -> s_win_*. */
static float             s_acc_sweep_min_ms = 0, s_acc_sweep_max_ms = 0;
static volatile float    s_win_sweep_min_ms = 0, s_win_sweep_max_ms = 0;

/* Engine-off quiesce state (Stage 1). Single-writer (the poll task) aligned fields, same no-mutex
 * contract as the status snapshot above; the getters below do plain reads. */
static volatile bool    s_bus_normal     = true;  /* default true: non-poll modes never suppress logging */
static volatile bool    s_quiesced       = false; /* true == bus currently flipped to LISTEN_ONLY */
static volatile int64_t s_last_ok_us     = 0;     /* esp_timer stamp of last matched OK reply (real ECU answer) */
/* The ECU sent back a matching reply to a request WE transmitted, in the current NORMAL session.
 * Renamed from s_confirmed, which never said what was confirmed. Read it as "somebody is really
 * answering us", i.e. THE IGNITION IS ON -- not "the engine is turning": a car at key-on with the
 * engine off answers every request. Set only on a matched reply (polllog_match), cleared by the
 * 5 s quiesce and by every probe resume. This is the signal the #4 sleep veto is built on. */
static volatile bool    s_ecu_answering  = false;
static volatile int64_t s_norm_start_us  = 0;     /* when the current NORMAL session began (boot/resume) = probe-window ref */
static volatile int64_t s_last_rx_us     = 0;     /* esp_timer stamp of last received frame (any id) */

/* Recording gate (WATCH vs FAST). Same single-writer/no-mutex contract as the fields above.
 * s_gate_open is what the CSV logger now gates on; s_bus_normal means only "not quiesced", and
 * /poll_status reports s_ecu_answering, so all three stay separate questions. */
static volatile bool     s_gate_open = false;
static float             s_gate_volt_on = POLLLOG_GATE_VOLT_DEF;  /* engine_volt, read once at init */
/* Last RPM seen on EITHER path (polled or broadcast). 32-bit on purpose: a 64-bit volatile is
 * not atomic on this core and could tear across the httpd/poll task boundary. s_rpm_seen stays
 * false when the table carries no RPM channel at all -- that is how the gate knows to drop the
 * RPM term instead of refusing to open. */
static volatile bool     s_rpm_seen  = false;
static volatile float    s_rpm_value = 0;
static volatile uint32_t s_rpm_ms    = 0;
static volatile int64_t s_last_flip_us   = 0;     /* dwell timer for POLLLOG_FLIP_MIN_MS */

/* Engine-running latch for the ENGINE_ON/ENGINE_OFF lines (see the block above). s_engine_on is
 * volatile because GET /poll_status reads it off the httpd task; the two timestamps are touched
 * only by the poll task and stay plain. */
static volatile bool    s_engine_on     = false;  /* true between ENGINE_ON and ENGINE_OFF */
static int64_t          s_engine_on_us  = 0;      /* when the current run started (for the duration) */
static int64_t          s_engine_low_us = 0;      /* first CONFIRMED under-threshold rpm; 0 = not armed */

/* Live per-row Test under POLL_LOG (issue #41). The httpd handler stages ONE request here and
 * blocks on s_test_done_sem; the poll task (sole TWAI consumer) picks it up at the top-of-loop
 * safe point -- AFTER can_should_park() -- runs it, and gives the semaphore back. s_test_req_mtx
 * admits one test at a time. The test is REFUSED (never queued) while a CSV trip is recording, so
 * it can never perturb a trip's frozen columns. */
static SemaphoreHandle_t s_test_req_mtx   = NULL;  /* one live test in flight */
static SemaphoreHandle_t s_test_done_sem  = NULL;  /* binary; poll task -> httpd task */
static volatile bool     s_test_requested = false;
static autopid_live_test_req_t s_test_req;
static autopid_live_test_res_t s_test_res;
#define POLLLOG_TEST_PID_RESP_MS   200   /* poll-task wait for the PID response (>> POLLLOG_RESP_TIMEOUT_MS) */
#define POLLLOG_TEST_CANFLT_MS     600   /* poll-task passive-capture window for a broadcast frame */

/*
 * Parse a polled-PID command string into request bytes.
 * The config stores cmd as the raw PID string plus a trailing CR, e.g. "010B1\r" (mode-01 PID 0B),
 * "2217461\r" (mode-22 PID 1746) or "23FFFFAC1800041\r" (mode-23 read of 4 bytes at 0xFFFFAC18 --
 * SID + 4-byte address + 2-byte size, issue #51). The LAST hex nibble is the ELM "expected response
 * frames" hint (1 = single frame here), NOT part of the request, so an odd nibble count means we
 * drop the final nibble. Returns request length in bytes (1..POLLLOG_MAX_REQ_BYTES), or 0 when the
 * row is NOT POLLABLE BY THIS PATH -- either the string isn't a PID request at all, or it is a
 * mode-23 read this single-frame receiver could never reassemble (see the shape gate at the end).
 * Longer strings are truncated to the cap rather than rejected, which is the pre-#51 behaviour
 * with a wider cap.
 */
static size_t polllog_req_bytes(const char *cmd, uint8_t out[POLLLOG_MAX_REQ_BYTES])
{
    if (!cmd)
        return 0;

    char hex[POLLLOG_MAX_REQ_BYTES * 2 + 1];   /* 14 request nibbles + the trailing frames hint */
    size_t n = 0;
    for (const char *p = cmd; *p && n < sizeof(hex); p++)
    {
        if (isxdigit((unsigned char)*p))
            hex[n++] = *p;
        else if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            continue;               /* ignore whitespace/CR */
        else
            break;                  /* non-hex terminator (e.g. an AT command) */
    }

    if (n & 1u)
        n--;                        /* drop the trailing nframes hint -> even nibble count */
    size_t bytes = n / 2;
    if (bytes < 1)
        return 0;
    if (bytes > POLLLOG_MAX_REQ_BYTES)
        bytes = POLLLOG_MAX_REQ_BYTES;

    for (size_t i = 0; i < bytes; i++)
    {
        char t[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        out[i] = (uint8_t)strtol(t, NULL, 16);
    }

    /* Mode 23 shape gate (issue #51). This is the ONE funnel every caller uses to decide
     * whether a row is pollable at all -- the sweep, polllog_prepare_schedule() and the live
     * PID test -- so rejecting here excludes a malformed mode-23 row uniformly instead of
     * letting it burn a phase slot and time out every sweep forever. Requires the exact
     * SID+addr4+size2 shape, and a size this single-frame path can actually receive: the
     * response is 0x63 + size bytes, so 1..POLLLOG_RMBA_MAX_SIZE. The web UI blocks both
     * cases at save time; this is the defensive floor for a hand-edited config. */
    if (out[0] == POLLLOG_SVC_RMBA)
    {
        /* Length FIRST: out[5]/out[6] are only written when the request is full length, so
         * reading the size out of a shorter one would be an indeterminate read of this buffer. */
        if (bytes != POLLLOG_RMBA_REQ_BYTES)
            return 0;
        const uint16_t size = polllog_rmba_size(out);
        if (size < 1 || size > POLLLOG_RMBA_MAX_SIZE)
            return 0;
    }
    return bytes;
}

/* True if msg is the PCM's single-frame POSITIVE response to the request bytes in req[]. */
static bool polllog_match(const twai_message_t *m, const uint8_t *req, size_t rl)
{
    if (m->identifier != POLLLOG_RX_ID || m->extd || m->rtr)
        return false;
    if (m->data_length_code < 3)
        return false;
    if ((m->data[0] & 0xF0u) != 0x00u)                 /* PCI type 0 -> single frame only */
        return false;
    if (m->data[1] != (uint8_t)(req[0] + 0x40u))       /* positive-response service echo */
        return false;
    /* Mode 23 (issue #51) answers 0x63 with NO address/size echo -- bench-verified -- so the
     * service byte is the only operand-independent thing to key on. Match the payload LENGTH
     * as well: the SF PCI low nibble is 1 (the 0x63 SID) + the requested size. That is free,
     * it validates the response really carries the bytes we asked for, and it discriminates
     * concurrent mode-23 channels of DIFFERENT sizes.
     *
     * KNOWN LIMIT: two mode-23 channels reading the SAME size are indistinguishable. Reaching a
     * misattribution needs a late reply to an already-TIMED-OUT request, because this poller
     * keeps one request in flight and drains stale frames immediately before every send; the
     * bench measures 0 timeouts over 25M requests. Modes 01/22 are immune -- their operand echo
     * disambiguates. Documented in docs/internals/poll_log.md. */
    if (req[0] == POLLLOG_SVC_RMBA)
        return (m->data[0] & 0x0Fu) == (uint8_t)(1u + polllog_rmba_size(req));
    if (rl >= 2 && m->data[2] != req[1])               /* PID echo (mode-01 PID / mode-22 hi) */
        return false;
    if (rl >= 3 && m->data[3] != req[2])               /* PID echo (mode-22 lo) */
        return false;
    return true;
}

/* Feed the gate's RPM input. Matched on the channel NAME, exactly and case-insensitively: both the
 * polled and the broadcast copy are named "RPM" in the NC table (the "[PID]" / "[CANFLT]" suffix is
 * added by the CSV writer, not part of the name). Exact rather than a prefix so a channel called
 * something like "RPM_target" cannot be mistaken for the real thing.
 *
 * A table with no RPM channel never reaches the assignment, s_rpm_seen stays false, and the gate
 * falls back to voltage alone -- exactly the pair that opens a CSV session today, so the gate can
 * never end up stricter than what already ships. */
static inline void polllog_stamp_rpm(const char *name, float value)
{
    if (!name || strcasecmp(name, "RPM") != 0)
        return;
    s_rpm_value = value;
    s_rpm_ms    = (uint32_t)(esp_timer_get_time() / 1000);
    s_rpm_seen  = true;
}

/* ---- ENGINE_ON / ENGINE_OFF emitters ----------------------------------------------------
 * Read the design block near POLLLOG_ENGINE_OFF_CONFIRM_MS before touching any of these.
 *
 * EVENT_LOG_DETAIL_MAX is 112 chars and truncation is SILENT (it has already cost one test round),
 * so both lines are counted at their worst case here:
 *   "engine started -- 99999 rpm, volts n/a"                              ->  38
 *   "engine stopped after 1193046h28m -- ECU stopped answering, volts n/a" ->  68
 * Both fit with room to spare. Every number that feeds them is CLAMPED below rather than trusted:
 * s_rpm_value is a decoded PID value and a broken expression could hand us 1e38, whose %.0f alone
 * is 39 characters. */

/* rpm as a printable integer. Clamped, and the !(>0) test also swallows NaN. */
static inline int polllog_rpm_i(float rpm)
{
    if (!(rpm > 0.0f))
        return 0;
    if (rpm > 99999.0f)
        return 99999;
    return (int)(rpm + 0.5f);
}

/* Battery volts for an event line, or the words "volts n/a". A queue peek (sleep_mode.c), so it is
 * cheap enough to pay on an edge -- and these run once per engine start, not per sweep. */
static void polllog_volt_str(char *out, size_t n)
{
    float v = 0;
    if (sleep_mode_get_voltage(&v) == ESP_OK)
        snprintf(out, n, "%.2fV", (double)v);
    else
        snprintf(out, n, "volts n/a");
}

/* Run length in words a human reads without dividing: "45s", "12m34s", "1h05m". */
static void polllog_dur_str(char *out, size_t n, uint32_t secs)
{
    if (secs >= 3600u)
        snprintf(out, n, "%uh%02um", (unsigned)(secs / 3600u), (unsigned)((secs / 60u) % 60u));
    else if (secs >= 60u)
        snprintf(out, n, "%um%02us", (unsigned)(secs / 60u), (unsigned)(secs % 60u));
    else
        snprintf(out, n, "%us", (unsigned)secs);
}

/* The rising edge: announce the engine ONCE. Returns immediately when already latched, which is
 * what keeps this off the per-sweep path -- every caller may call it unconditionally. */
static void polllog_engine_started(void)
{
    if (s_engine_on)
        return;
    s_engine_on     = true;
    s_engine_on_us  = esp_timer_get_time();
    s_engine_low_us = 0;

    char volts[12];
    polllog_volt_str(volts, sizeof(volts));
    ESP_LOGI(TAG, "engine started (%d rpm, %s)", polllog_rpm_i(s_rpm_value), volts);
    event_log_emit(EVL_ENGINE_ON, "engine started -- %d rpm, %s", polllog_rpm_i(s_rpm_value), volts);
}

/* The falling edge. `why` is the evidence, already in plain words ("0 rpm", "ECU stopped
 * answering"), because the two call sites know different things and neither reason should be
 * inferred from the other. No-op unless latched, so the quiesce path can call it blind. */
static void polllog_engine_stopped(const char *why)
{
    if (!s_engine_on)
        return;
    s_engine_on     = false;
    s_engine_low_us = 0;

    int64_t ran_us = esp_timer_get_time() - s_engine_on_us;
    if (ran_us < 0)
        ran_us = 0;
    char dur[16];
    polllog_dur_str(dur, sizeof(dur), (uint32_t)(ran_us / 1000000));
    char volts[12];
    polllog_volt_str(volts, sizeof(volts));
    ESP_LOGI(TAG, "engine stopped after %s (%s, %s)", dur, why, volts);
    event_log_emit(EVL_ENGINE_OFF, "engine stopped after %s -- %s, %s", dur, why, volts);
}

/* One pass of the engine-run edge, fed the gate's own rpm verdict so the two can never disagree.
 * Called from polllog_eval_gate ABOVE its CSV-session shortcut -- see the note there.
 *
 * The stop debounce is armed only by a KNOWN under-threshold reading, but once armed it is NOT
 * disarmed by the rpm going stale, and that asymmetry is load-bearing. At key-off the rpm samples
 * stop arriving about two seconds after the last one, which is BEFORE the 3 s debounce is up; if
 * staleness reset the timer, the normal way an engine stops would never complete the debounce and
 * the stop would only ever be reported by the quiesce path 5 s later. Only rpm back OVER the
 * threshold clears it -- i.e. only actual evidence that the engine is still turning.
 *
 * Conversely, staleness ALONE never arms it. A configuration that starves the rpm channel (a large
 * per-PID divisor, a dropped broadcast) must not be able to invent an engine stop mid-drive; in
 * that case the gate keeps recording (its rpm term drops out too) and the log stays quiet. */
static void polllog_engine_edge(int64_t now_us, bool rpm_known, bool rpm_running)
{
    if (rpm_known && rpm_running)
    {
        s_engine_low_us = 0;
        polllog_engine_started();
        return;
    }
    if (!s_engine_on)
        return;
    if (rpm_known && s_engine_low_us == 0)
        s_engine_low_us = now_us;
    if (s_engine_low_us == 0)
        return;
    if ((now_us - s_engine_low_us) <= (int64_t)POLLLOG_ENGINE_OFF_CONFIRM_MS * 1000)
        return;

    char why[16];
    snprintf(why, sizeof(why), "%d rpm", polllog_rpm_i(s_rpm_value));
    polllog_engine_stopped(why);
}

/* Decide whether the fast sweep is warranted right now. Runs once per sweep on the poll task,
 * above the sweep itself.
 *
 * "Running" = voltage at or above engine_volt AND (RPM over the threshold, when an RPM channel
 * exists). Opening measures against engine_volt; closing measures against engine_volt minus the
 * hysteresis band and must hold for POLLLOG_GATE_OFF_MS. Opening additionally needs s_ecu_answering,
 * so we never transmit flat out at a bus that has not answered.
 *
 * BOTH signals must agree to stay open, and EITHER one dropping closes the gate. That mirrors the
 * CSV writer, which records only while `ignition_on && engine_ok` (csv_logger.c) -- the gate is
 * open exactly when a trip could be recording, which is the whole point of having it.
 *
 * Getting this asymmetric was tempting and wrong. An earlier version let RPM alone close the gate
 * and ignored voltage, meaning to protect a live recording on a tired charging system that sags
 * under the band at idle. It protects nothing: the CSV writer decides on VOLTAGE alone, so it
 * closes that trip regardless (reason "ignition_off"), and the gate would then sit open at full
 * rate with nothing being recorded -- exactly the waste this state machine exists to remove.
 *
 * The CSV-session term is what keeps the web Start button and bench work at full rate: a manual
 * start opens a session from the slow watch records, this sees it and goes fast on the next pass.
 * A bench PCM reports RPM 0, so manual start is the only way to reach FAST there -- by design.
 *
 * vehicle_ignition_state() is deliberately NOT used: its hysteresis is a single static latched by
 * one caller (the CSV writer task, main/vehicle.c), so a second caller would race it. This applies
 * the same shared band (VEHICLE_IGN_HYSTERESIS_V) against its own state.
 *
 * An unreadable voltage does NOT hold the gate shut. Today's behaviour is "always fast", so
 * failing open is the only choice that cannot turn a broken ADC into silently lost data. With no
 * RPM channel configured the RPM term drops out and the gate becomes voltage-only -- the same
 * decision the CSV writer already makes, so it can never be stricter than what ships. */
static void polllog_eval_gate(int64_t now_us)
{
    static int64_t low_since_us = 0;   /* poll task only */

    /* RPM counts only while an RPM channel exists AND its last value is recent. Anything older
     * than two watch periods is treated as absent, not as zero.
     *
     * Computed FIRST, above every early return below, for the engine-run edge: the CSV-session
     * shortcut skips the rest of this function for the whole length of a trip, and the engine STOPS
     * during a trip -- evaluating the edge after that shortcut would mean ENGINE_OFF only ever
     * landed once the session had already closed. Pure reads, so hoisting it costs the shortcut
     * path a couple of comparisons and changes no gate behaviour. */
    bool rpm_known = false, rpm_running = false;
    if (s_rpm_seen)
    {
        const uint32_t now_ms = (uint32_t)(now_us / 1000);
        if ((uint32_t)(now_ms - s_rpm_ms) <= POLLLOG_RPM_STALE_MS)
        {
            rpm_known   = true;
            rpm_running = (s_rpm_value > POLLLOG_GATE_RPM_ON);
        }
    }
    polllog_engine_edge(now_us, rpm_known, rpm_running);

    if (csv_logger_session_active())
    {
        if (!s_gate_open)
        {
            s_gate_open  = true;
            ESP_LOGI(TAG, "recording gate OPEN (CSV session active) -> full-rate sweep");
        }
        low_since_us = 0;
        return;
    }

    float      volts  = 0;
    const bool have_v = (sleep_mode_get_voltage(&volts) == ESP_OK);

    /* One "is the engine running" answer, asked against a threshold that depends on which side of
     * the gate we are on: engine_volt to open, engine_volt minus the band to close. That band IS
     * the hysteresis, so a voltage hovering on the line cannot flap the gate. */
    const float volt_line = s_gate_open ? (s_gate_volt_on - POLLLOG_GATE_HYST_V) : s_gate_volt_on;
    const bool  volt_ok   = !have_v || (volts >= volt_line);
    const bool  rpm_ok    = !rpm_known || rpm_running;
    const bool  running   = volt_ok && rpm_ok;

    if (!s_gate_open)
    {
        if (s_ecu_answering && running)
        {
            s_gate_open  = true;
            low_since_us = 0;
            ESP_LOGI(TAG, "recording gate OPEN (%.2fV, rpm %s) -> full-rate sweep",
                     have_v ? (double)volts : 0.0,
                     rpm_known ? (rpm_running ? "running" : "stopped") : "n/a");
        }
        return;
    }

    if (running)
    {
        low_since_us = 0;          /* still running -> restart the debounce */
        return;
    }
    if (low_since_us == 0)
    {
        low_since_us = now_us;
        return;
    }
    if ((now_us - low_since_us) > (int64_t)POLLLOG_GATE_OFF_MS * 1000)
    {
        s_gate_open  = false;
        low_since_us = 0;
        ESP_LOGI(TAG, "recording gate CLOSED (%s for %dms) -> watch sweep every %dms",
                 !rpm_ok ? "rpm stopped" : "voltage under band",
                 POLLLOG_GATE_OFF_MS, POLLLOG_WATCH_SWEEP_MS);
    }
}

#if POLLLOG_HYBRID
/*
 * Hybrid: decode one drained NON-response frame against the configured broadcast filters and push
 * any matches to the wide CSV with source "CANFLT". Mirror of fast_log's fastlog_decode_frame(),
 * keyed to poll_log's own s_cfg. parameter_t.timer is repurposed as a per-channel throttle (the
 * AutoPID task isn't running in POLL_LOG, so that field is otherwise unused). Takes the same brief
 * autopid_lock as the response decode; never nested (a frame is EITHER our response OR broadcast).
 * A stale 0x7E8 response frame matches no can_filter, so it is correctly ignored here.
 */
static void polllog_decode_broadcast(const twai_message_t *msg)
{
    if (s_cfg == NULL || msg == NULL || msg->rtr)
        return;
    /* Try-lock (0): broadcast decode is opportunistic -- the next frame is ~20 ms away, so
     * skipping under contention is free. A blocking wait here would multiply per drained frame
     * inside the pre-send drain loop: an HTTP Test-PID/Test-filter handler holds this lock for
     * seconds, and at real bus rates that would stall ALL polling for the whole hold. */
    if (!autopid_lock(0))
        return;

    const bool extd = (msg->extd != 0);

    for (uint32_t fi = 0; fi < s_cfg->can_filters_count; fi++)
    {
        can_filter_t *f = &s_cfg->can_filters[fi];
        if (f->frame_id != msg->identifier || f->is_extended != extd)
            continue;
        /* Mirror the producer/column-provider gate (autopid.c): vehicle-profile filters decode
         * only while "Vehicle Specific PIDs" is enabled. Without this, every match would be a
         * column-less CSV record (cols_unmatched) burning record-queue slots at up to 50 Hz. */
        if (f->is_vehicle_specific && !s_cfg->pid_specific_en)
            continue;

        /* Timer read deferred to here: this function runs per drained frame at bus rate,
         * and most frames match no filter. */
        const int64_t now = esp_timer_get_time();

        /* evaluate_expression() does NOT bounds-check B0..B7: feed a full zero-padded 8-byte buf. */
        uint8_t buf[8] = {0};
        uint8_t n = msg->data_length_code;
        if (n > 8)
            n = 8;
        memcpy(buf, msg->data, n);

        for (uint32_t pi = 0; pi < f->parameters_count; pi++)
        {
            parameter_t *p = &f->parameters[pi];
            if (!p->enabled || p->expression == NULL || p->name == NULL || p->name[0] == '\0')
                continue;
            if (now < p->timer)                        /* per-channel throttle */
                continue;
            p->timer = now + (int64_t)POLLLOG_BCAST_PERIOD_MS * 1000;

            double result = 0;
            if (!evaluate_expression((uint8_t *)p->expression, buf, 0, &result))
                continue;
            if (!isfinite(result))
                continue;
            if (p->min != FLT_MAX && result < (double)p->min)
                continue;
            if (p->max != FLT_MAX && result > (double)p->max)
                continue;

            p->value = (float)result;
            polllog_stamp_rpm(p->name, p->value);   /* broadcast copy feeds the gate too */
            csv_logger_record(p->name, p->value, p->unit, "CANFLT");
        }
        /* no break: a frame_id may appear in more than one filter entry */
    }

    autopid_unlock();
}
#endif /* POLLLOG_HYBRID */

/* Sweep-divisor gate (issue #29). sample_every 0/1 == every sweep: one compare, no state
 * touched -> a config that sets nothing runs today's exact path, bit for bit.
 * Called at most ONCE per PID per EXECUTED sweep, and only while the gate is active;
 * see the invariant comment in polllog_rx_task. */
static inline bool polllog_pid_due(pid_data_t *pid)
{
    if (pid->sample_every <= 1)
        return true;
    if (pid->sample_ctr > 0)
    {
        pid->sample_ctr--;
        s_gate_skips++;
        return false;
    }
    pid->sample_ctr = (uint8_t)(pid->sample_every - 1);
    return true;
}

/*
 * Poll one PID: build the ISO-TP single-frame request, transmit, then wait (non-blocking drain)
 * up to POLLLOG_RESP_TIMEOUT_MS for the matching 0x7E8 reply. On a match, decode every enabled
 * parameter via evaluate_expression() on the RAW response bytes (B0=PCI, exactly the buffer shape
 * the expressions are authored against) and push to the wide CSV with source "PID" (matching the
 * column provider). Updates rolling turnaround stats.
 *
 * Returns true if a request was actually attempted -- which is exactly the condition under which
 * this call YIELDED and DRAINED (issue #29). The caller relies on that to detect a sweep in which
 * nothing was requested, because every vTaskDelay and every can_receive in the sweep lives here.
 */
static bool polllog_poll_one(pid_data_t *pid)
{
    if (!pid || !pid->enabled || !pid->cmd)
        return false;

    uint8_t req[POLLLOG_MAX_REQ_BYTES];
    size_t rl = polllog_req_bytes(pid->cmd, req);
    if (rl == 0)
    {
        /* DEBUG, not WARN: the sweep visits this row every pass (~45 Hz), so a warn here is a
         * continuous log flood for one bad row -- and at 2 Mbaud each line costs real time on
         * the sole-TWAI-owner task. The condition is reported ONCE per config load by
         * polllog_prepare_schedule() and published continuously as pids_unpollable in
         * /poll_status, which is the diagnostic surface that actually reaches the user. */
        ESP_LOGD(TAG, "skip PID with unparseable cmd '%s'", pid->cmd);
        return false;
    }

    /* Build the ISO-TP single-frame request (always an 8-byte OBD frame). */
    twai_message_t tx = {0};
    tx.identifier = POLLLOG_TX_ID;
    tx.data_length_code = 8;
    tx.data[0] = (uint8_t)rl;                 /* SF PCI = number of payload bytes */
    memcpy(&tx.data[1], req, rl);
    for (int i = 1 + (int)rl; i < 8; i++)
        tx.data[i] = POLLLOG_PAD;

    /* Drop any frames already queued so we time THIS request's response, not a stale one.
     * (Hybrid: decode each as broadcast first -- they're free fast-channel data, and decoding
     *  doesn't re-queue them, so the response timing below is unaffected.) */
    twai_message_t msg;
    while (can_receive(&msg, 0) == ESP_OK)
    {
#if POLLLOG_HYBRID
        polllog_decode_broadcast(&msg);
#endif
        /* discard for response-timing purposes */
    }

    const int64_t t_send = esp_timer_get_time();
    if (can_send(&tx, 0) != ESP_OK)           /* timeout 0: enqueue-or-fail, teardown-safe */
    {
        s_st.txfail++;
        s_cum_txfail++;
        vTaskDelay(1);  /* the only no-reply path that never waits: yield so a persistent TX-queue-full
                         * backlog (driver quiescing for OTA/sleep) can't spin this task -> WDT-safe */
        return true;    /* attempted (and yielded) -- not an empty-sweep iteration */
    }

    const int64_t deadline = t_send + (int64_t)POLLLOG_RESP_TIMEOUT_MS * 1000;
    bool got = false;
    while (esp_timer_get_time() < deadline)
    {
        while (can_receive(&msg, 0) == ESP_OK)        /* non-blocking drain */
        {
            if (!polllog_match(&msg, req, rl))
            {
#if POLLLOG_HYBRID
                polllog_decode_broadcast(&msg);        /* free broadcast channels while we wait */
#endif
                continue;                              /* not our response -> done with this frame */
            }

            const int64_t rtt = esp_timer_get_time() - t_send;

            if (autopid_lock(20))
            {
                for (uint32_t j = 0; j < pid->parameters_count; j++)
                {
                    parameter_t *p = &pid->parameters[j];
                    if (!p->enabled || !p->expression || !p->name || p->name[0] == '\0')
                        continue;
                    double result = 0;
                    if (!evaluate_expression((uint8_t *)p->expression, (uint8_t *)msg.data, 0, &result))
                        continue;
                    if (!isfinite(result))
                        continue;
                    if (p->min != FLT_MAX && result < (double)p->min)
                        continue;
                    if (p->max != FLT_MAX && result > (double)p->max)
                        continue;
                    p->value = (float)result;
                    polllog_stamp_rpm(p->name, p->value);
                    csv_logger_record(p->name, p->value, p->unit, "PID");
                }
                autopid_unlock();
            }

            s_st.sum_us += rtt;
            if (rtt < s_st.min_us) s_st.min_us = rtt;
            if (rtt > s_st.max_us) s_st.max_us = rtt;
            s_st.ok++;
            s_cum_ok++;
            s_last_ok_us = esp_timer_get_time();   /* engine-running heartbeat for the quiesce gate */
            if (!s_ecu_answering)
            {
                /* First real ECU answer of this NORMAL session => the ignition is confirmed on. A bus
                 * frame alone only PROBES (flips us to NORMAL); the OK is what confirms, so a stray
                 * wind-down frame can never log a false start. Exactly one IGNITION_ON per key-on.
                 * The message states the EVIDENCE ("ECU answering") because that is all we know: a
                 * car at key-on with the engine not turning answers every request too (#98). */
                s_ecu_answering = true;
                event_log_emit(EVL_IGNITION_ON, "ignition on -- ECU answering");
            }
            got = true;
            break;
        }
        if (got)
            break;
        vTaskDelay(1); /* 1 ms @ 1000 Hz tick: teardown-safe yield while awaiting the reply */
    }

    if (!got)
    {
        s_st.timeout++;
        s_cum_timeout++;
    }
    return true;
}

/* Assign each gated PID a phase offset so same-divisor channels spread across different
 * sweeps instead of all firing on the same one (which makes sweep duration oscillate).
 * Two-pass even spread WITHIN each divisor group. Also publishes the schedule summary read
 * by /poll_status and by the Auto CSV grid multiplier.
 *
 * Deterministic and RE-DERIVED (never preserved) across the live hot-reload: the same JSON
 * always yields the same phases, so a reload that changes an unrelated field causes no
 * disturbance, and there is no stable per-PID identity to match on anyway (the old table is
 * deep-freed, indices shift when a row is added, and cmd/name matching is a fragile O(n^2)
 * heuristic that can silently mis-map). Worst-case cost of re-deriving is one lcm(N)
 * transient, <= 64 sweeps. Deleting or reordering a gated row therefore re-phases its whole
 * divisor group -- harmless and deterministic, but it is why an unrelated edit can show up
 * as a brief sweep_min_ms/sweep_max_ms transient.
 *
 * KNOWN LIMITATION, deliberate: this equalises WITHIN a divisor group, not across groups, so
 * an N=2 group and an N=4 group still co-fire every 4th sweep. The within-group case is the
 * one that produces the pathological all-fire-together sweep. Residual oscillation is
 * MEASURABLE via sweep_min_ms/sweep_max_ms -- do not build a cross-N optimiser (bin-packing
 * over lcm(all N)) speculatively.
 */
static void polllog_prepare_schedule(autopid_config_t *c)
{
    uint8_t  cnt[AUTOPID_MAX_SAMPLE_EVERY + 1] = {0};
    uint8_t  k  [AUTOPID_MAX_SAMPLE_EVERY + 1] = {0};
    uint8_t  tmp[POLLLOG_MAX_REQ_BYTES];
    uint32_t gated = 0, min_n = 0, unpollable = 0;

    if (c == NULL)
    {
        s_pids_gated = 0;
        s_sched_min_n = 1;
        s_pids_unpollable = 0;
        return;
    }

    for (uint32_t i = 0; i < c->pid_count; i++)
    {
        pid_data_t *p = &c->pids[i];
        p->sample_ctr = 0;
        /* THE enforcement point for the divisor range. The parser clamps too, but THIS
         * function indexes cnt[]/k[] with the value and runs on the sole-TWAI-owner poll
         * task (8 KB internal-RAM stack), so an out-of-range value here is stack corruption
         * in the one task the brick-safety invariant depends on. Clamp and write back so
         * the gate and the schedule can never disagree. */
        if (p->sample_every > AUTOPID_MAX_SAMPLE_EVERY)
            p->sample_every = (uint8_t)AUTOPID_MAX_SAMPLE_EVERY;
        /* Exactly the validity test the sweep applies (polllog_poll_one): a row that is
         * disabled, has no cmd, or whose cmd never parses can NEVER produce a value. Such a
         * row must not pin sched_min_n (which would make the Auto CSV grid tick faster than
         * any channel refreshes and fill the log with LOCF duplicates) and must not consume
         * a phase slot. */
        if (!p->enabled || !p->cmd || polllog_req_bytes(p->cmd, tmp) == 0)
        {
            /* An ENABLED row the funnel refuses is a config error the user cannot otherwise
             * see: it is counted in `pids` but never in `sweep_pids`, indistinguishable from a
             * deliberately disabled row, and the only other signal is this log line on a device
             * with no serial console. Mode 23 adds two new ways to land here (wrong request
             * length, size outside 1..POLLLOG_RMBA_MAX_SIZE), so publish a count -- same
             * "no serial console needed" principle as pids_gated / sweep_empty / gate_skips. */
            if (p->enabled)
            {
                unpollable++;
                ESP_LOGW(TAG, "unpollable pid '%s': cmd missing or not a request this path can send",
                         p->cmd ? p->cmd : "(null)");
            }
            if (p->enabled && p->sample_every > 1)
                ESP_LOGW(TAG, "SampleEvery ignored: pid '%s' can never be polled", p->cmd ? p->cmd : "(null)");
            continue;
        }
        const uint8_t n = (p->sample_every > 1) ? p->sample_every : 1;
        if (min_n == 0 || n < min_n)
            min_n = n;
        if (n > 1 && cnt[n] < 255)
            cnt[n]++;
    }

    for (uint32_t i = 0; i < c->pid_count; i++)
    {
        pid_data_t *p = &c->pids[i];
        const uint8_t n = p->sample_every;
        if (!p->enabled || n <= 1 || cnt[n] == 0)
            continue;
        if (!p->cmd || polllog_req_bytes(p->cmd, tmp) == 0)
            continue;
        if (k[n] >= cnt[n])
            continue;   /* >255 PIDs in one divisor group: the tail keeps phase 0 (see cap comment) */
        /* k-th member of the N-group -> phase floor(k*N/cnt) mod N: even spread.
         * 2 PIDs at N=4 -> phases 0,2 (not 0,1 -- that is why it is two-pass);
         * 4 -> 0,1,2,3; 6 -> 0,0,1,2,2,3. */
        p->sample_ctr = (uint8_t)(((uint32_t)k[n] * n / cnt[n]) % n);
        k[n]++;
        gated++;
        ESP_LOGI(TAG, "  gated: '%s' every %u sweep(s), phase %u",
                 (p->parameters && p->parameters[0].name) ? p->parameters[0].name : p->cmd,
                 (unsigned)n, (unsigned)p->sample_ctr);
    }

    s_pids_gated      = gated;
    s_sched_min_n     = (min_n == 0) ? 1u : min_n;
    s_pids_unpollable = unpollable;

    if (unpollable > 0)
        ESP_LOGW(TAG, "%u enabled pid(s) are not pollable and were left out of the schedule",
                 (unsigned)unpollable);
    if (gated > 0)
        ESP_LOGI(TAG, "sweep divisors active: %u/%u pids gated, fastest channel every %u sweep(s)",
                 (unsigned)gated, (unsigned)c->pid_count, (unsigned)s_sched_min_n);
}

/* Live per-row Test executor (issue #41) -- runs ON the poll task (sole TWAI consumer), so it
 * borrows the adapter for exactly one request with no lock and no lease. It writes NO CSV and
 * mutates NO poll stats: expr/frame_id come from the copied request slot, not the swappable
 * s_cfg, so it needs no autopid_lock. Its caller places it BEHIND can_should_park(), so a
 * flash/park/host-claim always preempts it and no stray 0x7E0 is injected mid-ISO-TP. */
static void polllog_execute_test(const autopid_live_test_req_t *req, autopid_live_test_res_t *res)
{
    memset(res, 0, sizeof(*res));

    /* Trip-open TOCTOU re-check: a trip may have opened between the httpd fail-fast check and
     * this pickup. Refuse before any bus action so a trip's frozen columns are never perturbed. */
    if (csv_logger_session_active())
    {
        res->status = AUTOPID_TEST_TRIP_BUSY;
        return;
    }

    twai_message_t msg;

    if (req->kind == AUTOPID_LIVE_TEST_PID)
    {
        /* A PID test must transmit; if the bus is quiesced (engine/key off -> LISTEN_ONLY) we
         * cannot send. Report engine-off rather than flip the bus (anti-thrash). */
        if (s_quiesced || !s_bus_normal)
        {
            res->status = AUTOPID_TEST_ENGINE_OFF;
            return;
        }

        uint8_t reqb[POLLLOG_MAX_REQ_BYTES];
        size_t rl = polllog_req_bytes(req->cmd, reqb);
        if (rl == 0)
        {
            res->status = AUTOPID_TEST_DONE;
            snprintf(res->error, sizeof(res->error), "Unparseable PID command");
            return;
        }

        twai_message_t tx = {0};
        tx.identifier = POLLLOG_TX_ID;
        tx.data_length_code = 8;
        tx.data[0] = (uint8_t)rl;                 /* SF PCI = number of payload bytes */
        memcpy(&tx.data[1], reqb, rl);
        for (int i = 1 + (int)rl; i < 8; i++)
            tx.data[i] = POLLLOG_PAD;

        while (can_receive(&msg, 0) == ESP_OK) { /* drain stale frames -> time THIS response */ }

        if (can_send(&tx, 0) != ESP_OK)           /* timeout 0: enqueue-or-fail, teardown-safe */
        {
            res->status = AUTOPID_TEST_DONE;
            snprintf(res->error, sizeof(res->error), "TX failed (bus busy)");
            return;
        }

        const int64_t deadline = esp_timer_get_time() + (int64_t)POLLLOG_TEST_PID_RESP_MS * 1000;
        while (esp_timer_get_time() < deadline)
        {
            while (can_receive(&msg, 0) == ESP_OK)
            {
                if (!polllog_match(&msg, reqb, rl))
                    continue;                     /* not our positive response */
                double val = 0;
                bool ok = evaluate_expression((uint8_t *)req->expr, (uint8_t *)msg.data, 0, &val)
                          && isfinite(val);
                res->ok = ok;
                res->value = val;
                if (!ok)
                    snprintf(res->error, sizeof(res->error), "Expression did not evaluate");
                int n = 0;
                for (int b = 0; b < msg.data_length_code && n < (int)sizeof(res->raw) - 3; b++)
                    n += snprintf(res->raw + n, sizeof(res->raw) - n, "%02X ", msg.data[b]);
                res->status = AUTOPID_TEST_DONE;
                return;
            }
            vTaskDelay(1);                        /* WDT-safe: same non-blocking drain as the sweep */
        }
        res->status = AUTOPID_TEST_DONE;
        snprintf(res->error, sizeof(res->error), "Timeout - no response");
        return;
    }

    /* CANFLT: passive capture -- works even while quiesced (no transmit). */
    const int64_t deadline = esp_timer_get_time() + (int64_t)POLLLOG_TEST_CANFLT_MS * 1000;
    while (esp_timer_get_time() < deadline)
    {
        while (can_receive(&msg, 0) == ESP_OK)
        {
            if (msg.identifier != req->frame_id || ((msg.extd != 0) != req->is_extended) || msg.rtr)
                continue;
            uint8_t buf[8] = {0};                 /* evaluate_expression reads B0..B7 unchecked */
            uint8_t n8 = msg.data_length_code;
            if (n8 > 8) n8 = 8;
            memcpy(buf, msg.data, n8);
            double val = 0;
            bool ok = evaluate_expression((uint8_t *)req->expr, buf, 0, &val) && isfinite(val);
            res->ok = ok;
            res->value = val;
            if (!ok)
                snprintf(res->error, sizeof(res->error), "Expression did not evaluate");
            int m = 0;
            for (int b = 0; b < n8 && m < (int)sizeof(res->raw) - 3; b++)
                m += snprintf(res->raw + m, sizeof(res->raw) - m, "%02X ", buf[b]);
            res->status = AUTOPID_TEST_DONE;
            return;
        }
        vTaskDelay(1);
    }
    res->status = AUTOPID_TEST_DONE;
    snprintf(res->error, sizeof(res->error), "No matching frame seen");
}

/* Live per-row Test entry (issue #41) -- runs on the httpd task. Registered via
 * autopid_set_live_test_fn() and reached through autopid_live_test() from the test handlers.
 * Stages one request for the poll task and blocks up to timeout_ms for the result; refuses
 * (never queues) while a CSV trip records. */
static autopid_live_test_status_t polllog_live_test_entry(const autopid_live_test_req_t *req,
                                                          autopid_live_test_res_t *res,
                                                          uint32_t timeout_ms)
{
    memset(res, 0, sizeof(*res));

    if (!s_active || s_test_req_mtx == NULL || s_test_done_sem == NULL)
    {
        res->status = AUTOPID_TEST_INACTIVE;      /* POLL_LOG task not running */
        return res->status;
    }

    /* One test at a time. A short wait covers a concurrent tester; a longer stall means the poll
     * task is parked (flash) -- report busy rather than block the httpd worker indefinitely. */
    if (xSemaphoreTake(s_test_req_mtx, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        res->status = AUTOPID_TEST_TIMEOUT;
        return res->status;
    }

    /* Fail fast: do not even stage the request if a trip is recording. */
    if (csv_logger_session_active())
    {
        xSemaphoreGive(s_test_req_mtx);
        res->status = AUTOPID_TEST_TRIP_BUSY;
        return res->status;
    }

    /* Absorb any stale completion from a previous timed-out test before publishing this one. */
    xSemaphoreTake(s_test_done_sem, 0);
    s_test_req = *req;
    __sync_synchronize();                          /* payload committed before the flag is raised */
    s_test_requested = true;                       /* publish to the poll task */

    if (xSemaphoreTake(s_test_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        *res = s_test_res;
    }
    else
    {
        /* Timed out. The poll task can pick our request up LATE (it only runs after a possibly-
         * long can_should_park(), then the executor itself runs up to POLLLOG_TEST_CANFLT_MS), so
         * it may still be reading s_test_req / about to write s_test_res right now. Cancel it, then
         * -- STILL holding s_test_req_mtx so no other tester can reuse the shared slot -- wait
         * (bounded, generous) for a possible in-flight completion. This guarantees the executor is
         * done with s_test_req before we release the mutex (no torn read by the next tester) and
         * consumes its late give here so it can't be mis-delivered to that next tester (issue #41
         * review: cross-caller stale/torn result). If the request was never picked up, no give
         * comes and this simply expires; any ultra-late give is still absorbed by the pre-publish
         * drain above before the next request blocks. */
        s_test_requested = false;
        __sync_synchronize();
        (void)xSemaphoreTake(s_test_done_sem, pdMS_TO_TICKS(POLLLOG_TEST_CANFLT_MS * 2));
        res->status = AUTOPID_TEST_TIMEOUT;
    }

    xSemaphoreGive(s_test_req_mtx);
    return res->status;
}

static void polllog_rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "poll_log task started (NORMAL/on-bus, native-TWAI request/response, sole consumer)");
    s_active = true;   /* GET /poll_status now reports live counters */

    const int64_t start_us = esp_timer_get_time();
    s_norm_start_us = start_us;   /* boot enters NORMAL probing; arm the probe/off-detect window from t=0 */
    bool guard_cleared = false;
    int64_t stats_t = start_us;
    polllog_stats_reset();

    for (;;)
    {
        /* Single-CAN-owner interlock (task #36 / plan §5.3): the one TWAI controller is
         * reserved by ANY of a flash/read codec (FLASH_ACTIVE_BIT), a host REST datalog
         * pause, or a host bus-claim -- can_should_park() covers all three. Park here -- short
         * sleep + skip, NEVER portMAX_DELAY (that would starve the task WDT while the bus
         * is held). This stops a poll request (can_send below) from injecting a stray
         * 0x7E0 into the ECU's ISO-TP reassembly mid-TransferData (soft-brick) and stops
         * the bus disable/silent/enable flips from fighting the flash for the controller.
         * Resumes cleanly the moment the bus is released. */
        if (can_should_park())
        {
            vTaskDelay(pdMS_TO_TICKS(POLLLOG_FLASH_PARK_MS));
            continue;
        }

        /* Live PID-table hot-swap safe point (issue #39): we own the poll task and hold
         * no table pointer here (this is above the sweep). Do the swap here and nowhere
         * else. Defer while a CSV trip is open so the wide columns stay frozen for the
         * trip -- the flag stays set and drains at the first safe point after it closes. */
        if (s_reload_requested && !csv_logger_session_active())
        {
            s_reload_requested = false;
            autopid_config_t *n = autopid_reload_config();
            if (n == AUTOPID_RELOAD_DEFERRED)
            {
                /* A CSV trip opened between our check above and the swap (issue #43 P1).
                 * Not a rejection: re-arm so the swap retries at the next safe point once
                 * the trip closes. Leave s_last_reload_ok untouched (no outcome yet). */
                s_reload_requested = true;
            }
            else if (n != NULL)
            {
                s_cfg = n;
                /* Re-derive the divisor schedule for the NEW table (issue #29). Runs on the
                 * poll task, before this iteration's sweep, and nothing outside this task
                 * reads sample_ctr -- so the publish window is empty. Phases are deterministic
                 * from the JSON, so an unrelated edit re-derives them identically. */
                polllog_prepare_schedule(n);
                s_pid_count = n->pid_count;
                s_last_reload_ok = true;
                ESP_LOGI(TAG, "PID table hot-reloaded: %u pids", (unsigned)s_pid_count);
            }
            else
            {
                s_last_reload_ok = false;
                ESP_LOGW(TAG, "PID reload rejected -- keeping old table");
            }
        }

        /* Live per-row Test one-shot (issue #41). Same safe point as the reload above and,
         * critically, AFTER can_should_park(): a real flash/park/host-claim preempts the test so
         * no stray 0x7E0 lands mid-ISO-TP. Claim the flag BEFORE running so we stay the sole
         * consumer, then hand the result back to the blocked httpd task. Refused (not deferred)
         * under an open trip -- the executor re-checks csv_logger_session_active() itself. */
        if (s_test_requested)
        {
            s_test_requested = false;
            __sync_synchronize();                  /* read the payload only after observing the flag */
            polllog_execute_test(&s_test_req, &s_test_res);
            xSemaphoreGive(s_test_done_sem);
        }

        const int64_t now = esp_timer_get_time();

        if (s_bus_normal)
        {
            const uint32_t sweep_ok_before = s_cum_ok;
            const int64_t  sweep_t0 = now;

            /* Decide FAST vs WATCH before the sweep, so pacing and the divisor bypass below
             * both see one consistent answer for this pass. */
            polllog_eval_gate(now);
            /* WATCH == confirmed-answering but not worth recording. PROBE (!s_ecu_answering) is
             * deliberately excluded: it must stay full-rate or resume-in-one-frame breaks. */
            const bool watch_mode = s_ecu_answering && !s_gate_open;

            /* The divisor gate applies only when the ECU is confirmed answering AND an OK is
             * not going stale. Captured once so the whole sweep is consistent even if
             * s_ecu_answering flips mid-sweep.
             *   BYPASS 1 -- PROBING (!s_ecu_answering): boot and every quiesce-resume run full
             *     sweeps. Makes the probe path bit-identical to today and removes any risk
             *     that an all-gated table starves POLLLOG_PROBE_MS of poll attempts and
             *     strands the logger in a quiesce loop.
             *   BYPASS 2 -- STALE OK: once POLLLOG_GATE_STALE_MS has elapsed with no OK,
             *     poll everything. Without this, a sweep inflated by un-gated PIDs that
             *     always TIME OUT (30 ms each) combined with a high divisor on the only
             *     answering channel can push (now - s_last_ok_us) past POLLLOG_ENGINE_OFF_MS
             *     and fire a FALSE IGNITION_OFF, closing the csv require-engine gate mid-drive.
             *   BYPASS 3 -- WATCH (!s_gate_open): the slow sweep already runs at a fraction of
             *     the divisors' intended rate, so applying them on top would starve channels and
             *     leave the gate's own RPM input stale.
             * All three bypasses skip polllog_pid_due() entirely, so counters do NOT tick and
             * phase resumes exactly where it left off. */
            const bool gate_active = s_ecu_answering && s_gate_open &&
                ((now - s_last_ok_us) < (int64_t)POLLLOG_GATE_STALE_MS * 1000);
            /* "the sweep shape currently in effect is a GATED one". Drives both the EMA rule
             * and the fast-channel multiplier, so the measurement and the multiplier always
             * describe the same shape. False whenever nothing is gated -> every ungated
             * config takes today's exact path. */
            const bool gating_live = gate_active && (s_pids_gated > 0);
            s_gating_live = gating_live;
            uint32_t polled = 0;

            /* One single-PID round-robin sweep over all configured polled PIDs.
             * INVARIANT (issue #29): sample_ctr advances ONLY on an iteration that actually
             * reached this loop with the gate active. can_should_park() and the QUIESCED
             * branch both continue/delay above this point, so a 10 s flash session can never
             * burn every PID's skip budget and then fire them all at once on the first
             * unparked sweep. */
            for (uint32_t i = 0; i < s_cfg->pid_count; i++)
            {
                pid_data_t *p = &s_cfg->pids[i];
                if (gate_active && p->enabled && !polllog_pid_due(p))
                    continue;
                if (polllog_poll_one(p))
                    polled++;
                /* Phase B option (b): no per-PID delay. Every poll already yields inside
                 * polllog_poll_one -- the response-wait loop sleeps vTaskDelay(1) until the reply lands
                 * (the reply can't be queued before we send: we drain stale frames first), a timeout
                 * sleeps the full window, and the lone no-wait path (TX-queue-full) yields explicitly.
                 * So the idle task and the task watchdog stay fed without burning ~1 ms/PID of forced
                 * sleep -- reclaiming ~20% of the sweep time the measure-first run was leaving on the table. */
            }

            s_sweep_seq++;          /* the divisor's TIME BASE: one tick per gate pass */
            s_sweep_pids = polled;

            if (polled == 0)
            {
                /* Nothing was requested, so nothing yielded AND nothing drained: every
                 * vTaskDelay AND every can_receive in the sweep lives inside polllog_poll_one.
                 *  - Yield: a yield-free prio-5 loop starves IDLE (TWDT error flood; PANIC is
                 *    unset so it floods rather than reboots) and the prio-4 CSV writer.
                 *    taskYIELD() is NOT sufficient -- IDLE is prio 0 and this task would just
                 *    be re-selected.
                 *  - Drain: poll_log is the SOLE TWAI consumer. The driver RX queue is 96 slots
                 *    (main/can.c); on a 500 kbit powertrain bus at ~2000 frame/s it fills in
                 *    ~48 ms. A legal gated config (one PID at N=64) produces 63 consecutive
                 *    empty sweeps -- unbounded queue growth by construction. Draining here also
                 *    keeps hybrid broadcast decode running at the same rate it does today.
                 * Reachable from a legal gated config, and from a config in which every PID is
                 * disabled or has an unparseable cmd -- the latter spin already ships today. */
                s_sweep_empty++;
                twai_message_t m;
                while (can_receive(&m, 0) == ESP_OK)
                {
#if POLLLOG_HYBRID
                    polllog_decode_broadcast(&m);
#endif
                }
                /* Calculated channels on an empty sweep, so CALC keeps tracking the CANFLT
                 * inputs just decoded -- a broadcast-only config with all polled rows
                 * disabled still produces CALC columns. Unthrottled: the pacing floor below
                 * bounds this loop at POLLLOG_MIN_SWEEP_MS, so "every empty sweep" is <= 100
                 * Hz by construction. (Before the floor existed this path ran at ~1000 Hz and
                 * needed an explicit every-16th throttle to avoid flooding the record queue.) */
                autopid_eval_calculated_channels();
            }
            else
            {
                /* Calculated channels (Task #17): after a full round-robin sweep every polled source
                 * channel's p->value is freshest -- evaluate calc expressions over those and record each
                 * as source "CALC". No-op when none configured; takes autopid_lock itself (we hold none here). */
                autopid_eval_calculated_channels();
            }

            /* Hard rate cap: hold the sweep to POLLLOG_MIN_SWEEP_MS. See the constant.
             * Rounded UP so a paced sweep never lands under the floor. This is also the
             * ONLY guaranteed yield on the empty-sweep path (which is microseconds long, so
             * it always paces); the polled path additionally yields inside every
             * polllog_poll_one. The `polled == 0` fallback exists so that a future change
             * which lifts a sweep past the floor cannot silently produce a yield-free loop. */
            {
                const int64_t floor_us = watch_mode
                    ? (int64_t)POLLLOG_WATCH_SWEEP_MS * 1000
                    : POLLLOG_MIN_SWEEP_US;
                const int64_t raw_us = esp_timer_get_time() - sweep_t0;
                uint32_t pace_ms = 0;
                if (raw_us < floor_us)
                    pace_ms = (uint32_t)((floor_us - raw_us + 999) / 1000);
                if (pace_ms == 0 && polled == 0)
                    pace_ms = 1;
                if (pace_ms > 0)
                {
                    s_pace_sweeps++;
                    if (!watch_mode)
                    {
                        vTaskDelay(pdMS_TO_TICKS(pace_ms));
                    }
                    else
                    {
                        /* A watch wait is ~1 s, which is far too long to spend not draining: the
                         * RX queue overflows many times over at the frame rate quoted on the
                         * empty-sweep drain above. It would also blind the broadcast decode, which
                         * feeds the gate its own RPM input -- the thing that decides when to go
                         * fast. So the wait is chunked: sleep a little, drain, repeat. Same 20 ms
                         * cadence the QUIESCED branch uses, which also keeps can_should_park()
                         * reaction quick. */
                        uint32_t left = pace_ms;
                        while (left > 0 && !can_should_park())
                        {
                            const uint32_t step = (left > POLLLOG_WATCH_CHUNK_MS)
                                                ? POLLLOG_WATCH_CHUNK_MS : left;
                            vTaskDelay(pdMS_TO_TICKS(step));
                            left -= step;
                            twai_message_t m;
                            bool got_frame = false;
                            while (can_receive(&m, 0) == ESP_OK)
                            {
                                got_frame = true;
#if POLLLOG_HYBRID
                                polllog_decode_broadcast(&m);
#endif
                            }
                            /* Stamped once per chunk, not once per frame: at bus rate that is 50
                             * timer reads a second instead of ~2000, and the stamp can only be up
                             * to one 20 ms chunk old -- invisible to its readers, which work in
                             * hundreds of ms (poll_log_bus_idle_ms) and seconds (the quiesce
                             * detector). Same shape as the QUIESCED branch below. */
                            if (got_frame)
                                s_last_rx_us = esp_timer_get_time();
                        }
                    }
                }
            }

            /* Sweep-rate measurement (issue #23, extended by #29).
             * Fold rule: today's rule (">=1 OK") OR gating_live. The extra term is what makes
             * fast_ms EXACT: with divisors, some executed sweeps request nothing, and a PID
             * with divisor m fires once every m EXECUTED sweeps -- so its mean inter-sample
             * interval is m x (mean duration of ALL executed sweeps). Averaging only the
             * non-empty ones overstates it (worked example: 1 PID at N=2 + 1 at N=3 -> the
             * non-empty mean is 3.125 ms, x2 = 6.25 ms, but the N=2 channel's true mean
             * interval is 4.83 ms -- 29% high). Empty sweeps are folded ONLY while gating is
             * live, so the anti-poison guard (probe sweeps against a silent ECU) and the
             * all-PIDs-disabled case keep today's exact behaviour.
             * WATCH sweeps are excluded outright: folding a 1 s sweep in would hand the Auto CSV
             * grid a ~1 Hz rate and fill the next trip with duplicate rows. s_fast_hz instead
             * FREEZES at the last FAST measurement across watch and quiesce -- the same thing it
             * already does across a quiesce today, and accurate because the table has not
             * changed. First boot is the one exception: nothing has been measured yet, so the
             * grid starts at its 10 Hz default and converges within ~8 sweeps once FAST begins. */
            if (s_gate_open && ((s_cum_ok != sweep_ok_before) || gating_live))
            {
                static float ema_us = 0;   /* poll-task-local; the volatiles below are the readers' view */
                static bool  ema_gated = false;
                if (ema_gated != gating_live)
                {
                    /* The sweep SHAPE just changed (gate engaged/disengaged: probe->confirmed,
                     * stale bypass, or a hot-reload that added/removed divisors). Re-seed
                     * rather than decay: an alpha-1/8 EMA needs ~8 sweeps to cross, and the
                     * multiplier flips instantly, so decaying would hand csv_grid_period_ms()
                     * a rate that is wrong by up to min_n x for ~0.4 s on EVERY engine restart. */
                    ema_gated = gating_live;
                    ema_us = 0;
                }
                const float sweep_us = (float)(esp_timer_get_time() - sweep_t0);
                ema_us = (ema_us > 0) ? (ema_us * 0.875f + sweep_us * 0.125f) : sweep_us;
                s_sweep_ms = ema_us / 1000.0f;
                s_sweep_hz = (ema_us > 0) ? (1e6f / ema_us) : 0;
                /* Fastest channel's cadence = mean executed-sweep x smallest live divisor.
                 * mn is 1 whenever the gate is not actually in effect, so the multiplier and
                 * the measurement always describe the same sweep shape. Identical to sweep_ms
                 * when nothing is gated. THIS -- not sweep_ms -- is what the Auto CSV grid tracks. */
                const float mn = gating_live ? (float)(s_sched_min_n ? s_sched_min_n : 1u) : 1.0f;
                s_fast_ms = s_sweep_ms * mn;
                s_fast_hz = s_sweep_hz / mn;
                /* 3 s-window sweep-duration spread: the phasing-quality readout. */
                const float ms = sweep_us / 1000.0f;
                if (s_acc_sweep_min_ms == 0 || ms < s_acc_sweep_min_ms) s_acc_sweep_min_ms = ms;
                if (ms > s_acc_sweep_max_ms) s_acc_sweep_max_ms = ms;
            }

            /* Quiesce decision -> flip the bus to LISTEN_ONLY so we stop holding it awake. Two cases,
             * so we never spin-transmit onto a dead bus:
             *   - CONFIRMED answering (had an OK this session): quiesce when the ECU is silent for
             *     ENGINE_OFF_MS -> log IGNITION_OFF.
             *   - PROBING (booted, or resumed on a bus frame, but no OK yet): re-quiesce after the short
             *     PROBE_MS window. This kills a boot with the key off (or a stray wind-down frame) that
             *     would otherwise transmit failed requests forever (the old 100k+ txfail spin). No
             *     IGNITION_OFF is logged -- the ignition was never confirmed on.
             * The flip MUST bracket can_set_silent() with disable/enable -- it is a no-op while ON_BUS. */
            bool engine_off = s_ecu_answering
                ? ((now - s_last_ok_us)    > (int64_t)POLLLOG_ENGINE_OFF_MS * 1000)
                : ((now - s_norm_start_us) > (int64_t)POLLLOG_PROBE_MS      * 1000);
            if (engine_off && (now - s_last_flip_us) > (int64_t)POLLLOG_FLIP_MIN_MS * 1000)
            {
                bool was_confirmed = s_ecu_answering;
                can_disable();
                can_set_silent(1);
                can_enable();
                if (can_is_enabled())
                {
                    s_quiesced       = true;
                    s_bus_normal     = false;
                    s_ecu_answering  = false;
                    s_gate_open      = false;  /* ECU gone -> nothing to record; re-decide on resume */
                    s_last_rx_us     = now;   /* arm the idle clock from the flip instant */
                    s_last_flip_us   = now;
                    /* A silent ECU is proof the engine is not turning: an engine cannot run with
                     * its PCM off the bus. This is the SECOND stop path and the one that normally
                     * fires -- at key-off the rpm samples stop before the rpm-based debounce in
                     * polllog_engine_edge completes, unless the ECU keeps answering through the
                     * spin-down. Called unconditionally (it is a no-op unless the latch is up) so
                     * the latch cannot survive a quiesce and silence the NEXT start. Emitted BEFORE
                     * IGNITION_OFF because that is the real order of events: the engine stops, then
                     * the ignition goes. No extra debounce -- POLLLOG_ENGINE_OFF_MS already waited
                     * 5 s of silence to get here. */
                    polllog_engine_stopped("ECU stopped answering");
                    if (was_confirmed)
                    {
                        ESP_LOGI(TAG, "ECU silent %dms -> LISTEN_ONLY quiesce (stop holding bus awake)",
                                 POLLLOG_ENGINE_OFF_MS);
                        /* Operational event (Task #24): once per confirmed on->off transition.
                         * Plain words, not internals: "5000ms -> quiesce (LISTEN_ONLY)" means nothing
                         * to someone reading their own event log (#98). The seconds come from the
                         * constant so the text and the timeout can never drift apart. */
                        event_log_emit(EVL_IGNITION_OFF,
                                       "ignition off -- no ECU reply for %ds, stopped sending requests",
                                       POLLLOG_ENGINE_OFF_MS / 1000);
                    }
                    else
                    {
                        ESP_LOGI(TAG, "no ECU reply within %dms -> LISTEN_ONLY quiesce (engine off, probe)",
                                 POLLLOG_PROBE_MS);
                    }
                }
                /* if !can_is_enabled(): an OTA/sleep fence disabled the bus under us -> just fall
                 * through; the next iteration re-checks and we never spin against a dead driver. */
            }
        }
        else
        {
            /* QUIESCED (LISTEN_ONLY): never transmit. Drain RX non-blocking; the FIRST frame of any
             * id means the car woke. Resume to NORMAL (same mandatory disable/silent/enable bracket),
             * or after MAX_QUIESCE as a self-heal so a stuck detector can never strand the logger. */
            twai_message_t m;
            int got = 0;
            while (can_receive(&m, 0) == ESP_OK)
            {
                s_last_rx_us = now;
                got++;
            }

            bool resume = (got >= POLLLOG_RESUME_FRAMES) ||
                          ((now - s_last_flip_us) > (int64_t)POLLLOG_MAX_QUIESCE_MS * 1000);

            if (resume && (now - s_last_flip_us) > (int64_t)POLLLOG_FLIP_MIN_MS * 1000)
            {
                can_disable();
                can_set_silent(0);
                can_enable();
                if (can_is_enabled())
                {
                    s_quiesced       = false;
                    s_bus_normal     = true;
                    s_ecu_answering  = false; /* PROBE: a frame woke us, but require a real OK to confirm running */
                    s_norm_start_us  = now;   /* start the probe window (re-quiesce after PROBE_MS if no OK) */
                    s_last_flip_us   = now;
                    ESP_LOGI(TAG, "bus alive (%d frame[s]) -> NORMAL, probing for ECU", got);
                    /* IGNITION_ON is emitted on the first OK (polllog_poll_one), NOT here: a stray
                     * wind-down frame that yields no OK is a false alarm and must not log a start. */
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));   /* ~20 ms quiesce cadence: feeds WDT/idle, fast resume */
        }

        if (!guard_cleared && (esp_timer_get_time() - start_us) > POLLLOG_GUARD_STABLE_US)
        {
            s_polllog_guard = 0;
            guard_cleared = true;
            ESP_LOGI(TAG, "poll_log stable; crash-guard cleared");
        }

        if (now - stats_t > POLLLOG_STATS_PERIOD_US)
        {
            const int total = s_st.ok + s_st.timeout + s_st.txfail;
            const float window_s = (float)(now - stats_t) / 1e6f;
            const float avg_ms = s_st.ok ? ((float)s_st.sum_us / (float)s_st.ok) / 1000.0f : 0.0f;
            const float min_ms = (s_st.ok && s_st.min_us != INT64_MAX) ? (float)s_st.min_us / 1000.0f : 0.0f;
            const float max_ms = (float)s_st.max_us / 1000.0f;
            ESP_LOGI(TAG,
                     "poll stats: ok=%d timeout=%d txfail=%d | rtt avg=%.2f min=%.2f max=%.2f ms | %.0f req/s",
                     s_st.ok, s_st.timeout, s_st.txfail, avg_ms, min_ms, max_ms,
                     window_s > 0 ? total / window_s : 0.0f);
            /* Publish this window's metrics for GET /poll_status (the cumulative ok/timeout/txfail
             * counters already update live on every poll; only the rate/rtt view is windowed). */
            s_win_ok = s_st.ok;
            s_win_timeout = s_st.timeout;
            s_win_txfail = s_st.txfail;
            s_win_rtt_avg_ms = avg_ms;
            s_win_rtt_min_ms = min_ms;
            s_win_rtt_max_ms = max_ms;
            s_win_req_s = (window_s > 0) ? (total / window_s) : 0.0f;
            /* Publish the sweep-duration spread mirrors (issue #29) and re-arm the working
             * accumulators. Done HERE, above polllog_stats_reset(), so /poll_status only ever
             * reads a completed window -- never a mid-window accumulator that could report a
             * 0.0 ms sweep and make a phasing check pass for the wrong reason. */
            s_win_sweep_min_ms = s_acc_sweep_min_ms;
            s_win_sweep_max_ms = s_acc_sweep_max_ms;
            s_acc_sweep_min_ms = 0;
            s_acc_sweep_max_ms = 0;
            polllog_stats_reset();
            stats_t = now;
        }
    }
}

bool poll_log_bringup_skipped(void)
{
    return s_bringup_skipped;
}

void poll_log_init(char *id, uint32_t log_period)
{
    (void)id;
    (void)log_period;

    /* ---- One-shot crash-guard ------------------------------------------- */
    if (s_polllog_guard == POLLLOG_GUARD_ARMED)
    {
        s_polllog_guard  = 0; /* disarm so the next boot retries */
        s_bringup_skipped = true;
        ESP_LOGW(TAG, "crash-guard was armed; skipping POLL_LOG bring-up this boot");
        return;
    }
    s_polllog_guard = POLLLOG_GUARD_ARMED;

    /* ---- Load the channel config (no AutoPID task, no ELM polling) ------- */
    s_cfg = autopid_load_config_only();
    if (s_cfg == NULL)
    {
        ESP_LOGE(TAG, "no config; POLL_LOG inactive");
        s_polllog_guard = 0;
        return;
    }
    s_pid_count = s_cfg->pid_count;   /* cross-task mirror for GET /poll_status (issue #39) */
    /* Seed the divisor schedule (issue #29) before the poll task exists -- no race. */
    polllog_prepare_schedule(s_cfg);
    if (s_cfg->pid_count == 0)
    {
        ESP_LOGW(TAG, "config has 0 polled PIDs; POLL_LOG is poll-only, nothing to request");
        s_polllog_guard = 0;
        return;
    }

#if POLLLOG_HYBRID
    /* Reset the per-param broadcast throttle timestamps repurposed in polllog_decode_broadcast(). */
    for (uint32_t fi = 0; fi < s_cfg->can_filters_count; fi++)
        for (uint32_t pi = 0; pi < s_cfg->can_filters[fi].parameters_count; pi++)
            s_cfg->can_filters[fi].parameters[pi].timer = 0;
#endif

    /* ---- Bring up native TWAI in NORMAL/on-bus mode (poll_log TRANSMITS) --- */
    /* can_set_silent(0) = NORMAL: unlike fast_log's LISTEN_ONLY, we must be on-bus to send
     * requests and to ACK the PCM's responses. Must precede can_enable() (no-op while ON_BUS). */
    can_set_silent(0);
    int rate = config_server_get_can_rate();
    can_set_bitrate((rate >= 0) ? (uint8_t)rate : (uint8_t)CAN_500K);
    can_enable();
    if (!can_is_enabled())
    {
        ESP_LOGE(TAG, "can_enable failed; POLL_LOG inactive");
        s_polllog_guard = 0;
        return;
    }

    /* ---- Live-test one-shot handshake (issue #41): create BEFORE the task exists ------- */
    s_test_req_mtx  = xSemaphoreCreateMutex();
    s_test_done_sem = xSemaphoreCreateBinary();

    /* ---- Create the sole-consumer poll task ----------------------------- */
    TaskHandle_t h = xTaskCreateStatic(polllog_rx_task, "polllog_rx",
                                       POLLLOG_RX_STACK_BYTES, NULL, POLLLOG_RX_TASK_PRIO,
                                       s_rx_task_stack, &s_rx_task_buf);
    if (h == NULL)
    {
        ESP_LOGE(TAG, "failed to create poll task; POLL_LOG inactive");
        s_polllog_guard = 0;
        return;
    }

    /* Let the CSV logger gate on engine-running via our CAN-derived signal. Registration (not a
     * direct include) avoids a circular component dependency: poll_log already depends on
     * csv_logger, not the reverse. Registers the RECORDING gate, not poll_log_ignition_on --
     * see the WATCH vs FAST block near the top for why those are different questions. */
    csv_logger_set_engine_state_fn(poll_log_gate_open);

    /* The gate's voltage threshold. Read once here rather than per sweep: changing engine_volt
     * goes through /store_config, which reboots, so it cannot change under a running task. */
    if (config_server_get_engine_volt(&s_gate_volt_on) == -1)
    {
        s_gate_volt_on = POLLLOG_GATE_VOLT_DEF;
        ESP_LOGW(TAG, "engine_volt unreadable; recording gate uses %.1fV", (double)s_gate_volt_on);
    }
    ESP_LOGI(TAG, "recording gate: >= %.1fV (plus RPM when configured); watch sweep every %dms",
             (double)s_gate_volt_on, POLLLOG_WATCH_SWEEP_MS);

    /* Same registration pattern for the measured sweep rate (issue #23): the CSV writer's
     * "Auto" fixed-rate grid tracks poll_log's real sweep frequency with no reverse dep. */
    csv_logger_set_rate_fn(poll_log_sweep_hz);

    /* Live per-row Test under POLL_LOG (issue #41): expose our in-band one-shot executor to the
     * /autopid/test_pid and /autopid/test_can_filter handlers via the autopid registry -- no
     * reverse fast_log dependency, same registration pattern as the two calls above. */
    autopid_set_live_test_fn(polllog_live_test_entry);

    ESP_LOGI(TAG, "Phase B (measure-first) up: %lu polled PID(s), NORMAL/on-bus, single-PID round-robin",
             (unsigned long)s_cfg->pid_count);
}

/* Ignition/quiesce state for the CSV logging gate, the Route-B sleep sensor, and /poll_status.
 * Plain reads of the poll task's aligned fields (no mutex, same contract as the status snapshot).
 * When POLL_LOG is not the active mode these report "ignition on / not idle" so other modes
 * (FAST_LOG, bench) and any stale read never suppress logging or wrongly trigger sleep.
 *
 * DO NOT MERGE THIS WITH poll_log_ecu_answering() BELOW -- opposite failure directions, and one of
 * them guards the car battery. Read the DO-NOT-MERGE banner in poll_log.h first. */
bool poll_log_ignition_on(void)
{
    /* "confirmed answering" (the ECU replied to a poll this session), NOT merely in NORMAL mode:
     * during the ~2s probe after a boot or a stray-frame resume the ECU hasn't replied yet, so this
     * stays false. That keeps the csv require-engine gate closed during a probe AND makes
     * /poll_status honest -- a boot with the key off now reports ignition_on:false instead of the
     * old misleading true. */
    return s_active ? s_ecu_answering : true;
}

bool poll_log_quiesced(void)
{
    return s_active ? s_quiesced : false;
}

/* DO NOT MERGE THIS WITH poll_log_ignition_on() ABOVE. Since #98 the two names read alike; the
 * behaviour is still opposite. Read the DO-NOT-MERGE banner in poll_log.h first.
 *
 * Sleep veto (issue #4). TRUE only while ALL THREE hold:
 *   s_active           -- the poll task is running THIS uptime, so somebody maintains the rest,
 *   s_ecu_answering        -- the ECU answered one of OUR requests in the current session,
 *   !can_should_park() -- the poller is actually free to keep s_ecu_answering fresh.
 *
 * FAILS CLOSED (returns false) in every other situation, which is the OPPOSITE default from the
 * logging predicates above and is the whole reason this is not a wrapper around one of them:
 *   - a veto that reads true when nothing maintains it means the device NEVER SLEEPS. In
 *     ELM327/FAST_LOG mode, before the task starts, with a 0-PID table, or after the crash guard
 *     skipped bring-up, poll_log_ignition_on() returns TRUE (defined just above). Using it here would
 *     silently flatten the car battery, and a bench running POLL_LOG would never reveal it.
 *   - s_ecu_answering rather than s_bus_normal: s_bus_normal also goes true for up to ~2 s on
 *     ANY stray frame during a probe (:1417), so a chattering module that never answers a poll
 *     could hold the veto up indefinitely. s_ecu_answering cannot rise without a matched reply to a
 *     frame we transmitted (:715 -> :759).
 *   - !can_should_park(): while the poller is parked (flash, host claim, datalog pause, sleep
 *     fence) s_ecu_answering stops being updated and freezes at its last value. A frozen TRUE with no
 *     bound is a second forever-awake path -- notably if a flash lease is ever left raised, the
 *     case sleep_mode.c:973-976 calls out as having no reaper. Dropping the veto when parked hands
 *     the decision back to the teardown's own bounded interlock instead.
 *
 * Deliberately NOT here: any voltage term, any RPM term, any time cap. The owner's rule is that
 * an answering ECU means the ignition is on, and with the ignition on the car itself draws amps,
 * so bounding the dongle's tens of mA would buy nothing. s_gate_open (:320) is a voltage+RPM+CSV
 * composite and is wrong for this on all three counts. */
bool poll_log_ecu_answering(void)
{
    return s_active && s_ecu_answering && !can_should_park();
}

/* The RECORDING gate (see the WATCH vs FAST block near the top). Returns true when POLL_LOG is not
 * the active mode, so other modes and any stale read can never suppress logging. */
bool poll_log_gate_open(void)
{
    return s_active ? s_gate_open : true;
}

/* The ENGINE_ON/ENGINE_OFF latch, exposed so GET /poll_status can show the same fact the event log
 * just claimed -- the only way to check this feature live, since the bench PCM reports rpm 0 and
 * can never make it true. Returns FALSE outside POLL_LOG rather than the fail-open true that
 * poll_log_gate_open() uses: this is a report, not a permission, and "we are not measuring rpm"
 * must never read as "the engine is running". */
bool poll_log_engine_running(void)
{
    return s_active && s_engine_on;
}

float poll_log_sweep_hz(void)
{
    /* Issue #29: with per-PID divisors, mean sweep time is no longer the rate at which any
     * channel refreshes -- the fastest channel refreshes every s_sched_min_n sweeps.
     * Reporting mean sweep here would make the Auto grid oversample and emit full-width
     * duplicate rows. s_fast_hz == s_sweep_hz whenever gating is not live, so a config that
     * sets no SampleEvery feeds the CSV grid the identical float.
     * 0 = no measurement (POLL_LOG inactive, or no sweep has completed with an OK yet);
     * callers (the CSV auto-grid) fall back to their own default on 0. */
    return s_active ? s_fast_hz : 0.0f;
}

uint32_t poll_log_bus_idle_ms(void)
{
    if (!s_active || s_bus_normal)
        return UINT32_MAX;   /* not idle while actively polling or outside POLL_LOG */
    int64_t d = (esp_timer_get_time() - s_last_rx_us) / 1000;
    if (d < 0) d = 0;
    if (d > (int64_t)UINT32_MAX) d = UINT32_MAX;
    return (uint32_t)d;
}

/*
 * Build the WiFi-visible poll status as a small JSON object (caller free()s). Hand-rolled with
 * snprintf so the fast_log component needs no cJSON/json dependency. ok/timeout/txfail are
 * cumulative since the task started; rtt_* and req_s are the last 3 s window; win_* is that
 * window's poll counts. Returns "active":false with zeroed fields when POLL_LOG never ran, so
 * the endpoint is safe to call in any protocol mode.
 */
char *poll_log_get_status_json(void)
{
    char *buf = malloc(POLLLOG_STATUS_JSON_SZ);
    if (buf == NULL)
        return NULL;

    /* One word for what the poll task is doing, because req_s alone is now ambiguous: ~16/s is
     * healthy in watch and alarming in fast. "probe" is the short window after a boot or a resume
     * where the ECU has not answered yet -- still full rate, by design. */
    const char *state = !s_active      ? "inactive"
                      : s_quiesced     ? "quiesced"
                      : !s_ecu_answering   ? "probe"
                      : s_gate_open    ? "fast"
                                       : "watch";

    int n = snprintf(buf, POLLLOG_STATUS_JSON_SZ,
             "{\"active\":%s,\"ok\":%u,\"timeout\":%u,\"txfail\":%u,"
             "\"rtt_avg_ms\":%.2f,\"rtt_min_ms\":%.2f,\"rtt_max_ms\":%.2f,\"req_s\":%.1f,"
             "\"sweep_ms\":%.1f,\"sweep_hz\":%.2f,\"pids\":%u,"
             "\"pids_gated\":%u,\"pids_unpollable\":%u,\"sched_min_n\":%u,\"gating_active\":%s,"
             "\"sweep_pids\":%u,\"sweep_seq\":%u,\"sweep_empty\":%u,\"gate_skips\":%u,"
             "\"pace_sweeps\":%u,\"min_sweep_ms\":%u,"
             "\"sweep_min_ms\":%.1f,\"sweep_max_ms\":%.1f,\"fast_ms\":%.1f,\"fast_hz\":%.2f,"
             "\"win_ok\":%u,\"win_timeout\":%u,\"win_txfail\":%u,"
             "\"ignition_on\":%s,\"quiesced\":%s,\"bus_idle_ms\":%u,\"reload_ok\":%s,"
             "\"reload_pending\":%s,"
             "\"state\":\"%s\",\"gate_open\":%s,"
             "\"gate_volt\":%.1f,\"rpm_known\":%s,\"rpm\":%.0f,\"engine_running\":%s}",
             s_active ? "true" : "false",
             (unsigned)s_cum_ok, (unsigned)s_cum_timeout, (unsigned)s_cum_txfail,
             (double)s_win_rtt_avg_ms, (double)s_win_rtt_min_ms, (double)s_win_rtt_max_ms,
             (double)s_win_req_s,
             (double)s_sweep_ms, (double)s_sweep_hz,
             (unsigned)s_pid_count,   /* cross-task-safe mirror, never derefs s_cfg (issue #39) */
             (unsigned)s_pids_gated, (unsigned)s_pids_unpollable, (unsigned)s_sched_min_n,
             s_gating_live ? "true" : "false",
             (unsigned)s_sweep_pids, (unsigned)s_sweep_seq, (unsigned)s_sweep_empty,
             (unsigned)s_gate_skips,
             (unsigned)s_pace_sweeps, (unsigned)POLLLOG_MIN_SWEEP_MS,
             (double)s_win_sweep_min_ms, (double)s_win_sweep_max_ms,
             (double)s_fast_ms, (double)s_fast_hz,
             (unsigned)s_win_ok, (unsigned)s_win_timeout, (unsigned)s_win_txfail,
             poll_log_ignition_on() ? "true" : "false",
             s_quiesced ? "true" : "false",
             (unsigned)poll_log_bus_idle_ms(),
             s_last_reload_ok ? "true" : "false",
             s_reload_requested ? "true" : "false",
             state,
             s_gate_open ? "true" : "false",
             (double)s_gate_volt_on,
             s_rpm_seen ? "true" : "false",
             (double)s_rpm_value,
             poll_log_engine_running() ? "true" : "false");
    /* Silent truncation would emit INVALID JSON to the web UI and to any tooling polling
     * this endpoint -- log loudly rather than let a future field addition break it quietly. */
    if (n < 0 || n >= POLLLOG_STATUS_JSON_SZ)
        ESP_LOGE(TAG, "poll_status JSON truncated (%d >= %d) -- invalid JSON emitted",
                 n, POLLLOG_STATUS_JSON_SZ);
    return buf;
}

/* Live PID-table hot-swap request (issue #39). Called on the httpd task by
 * store_auto_data_handler after it writes the new auto_pid.json. Sets a flag ONLY; the
 * actual re-parse + swap runs on the poll task at its top-of-loop safe point. s_active
 * doubles as the mode guard: it is only true while POLL_LOG's task is running, so
 * non-POLL_LOG modes (AUTO_PID/FAST_LOG) return false and the handler answers
 * "reboot-required" -- the only correct answer when this task isn't the one owning the
 * table. Returns true if the reload was queued. */
bool poll_log_request_reload(void)
{
    if (!s_active)
        return false;
    s_reload_requested = true;
    return true;
}
