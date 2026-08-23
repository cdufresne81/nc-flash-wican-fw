/*
 * This file is part of the WiCAN project.
 *
 * Copyright (C) 2022  Meatpi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// SD directory holding the rotating event-log files. Hardcoded mount root MUST match
// SD_CARD_MOUNT_POINT in main/sdcard.h (event_log is a leaf and can't read that macro). Exposed here
// so sd_filemgr can treat it as a protected dir.
#define EVENT_LOG_DIR  "/sdcard/events"

// On-device operational event log (Task #24).
//
// Records meaningful operating events (boot, engine start/stop, datalog session open/close,
// software update / reboot, and NC-Flash flash/read + host-coexistence sessions) so the device
// keeps a factual, retrievable history of what it did -- without a serial cable. This matters most
// on the wireless brick-risk path: a flash leaves no other post-hoc trace. Two sinks: an
// always-present in-RAM ring (survives a missing/failed SD) and a rotating text file on the SD card.
//
// BRICK-SAFE CONTRACT (mirrors csv_logger's invariants):
//  - This is a LEAF component: it REQUIRES only base IDF, so csv_logger / poll_log / config_server /
//    main can all call event_log_emit() without a dependency cycle.
//  - event_log_emit() is NON-BLOCKING and safe from any task (incl. the poll_log poll loop at prio 5
//    and the csv_logger writer at prio 4): it formats into a stack buffer, copies one line into the
//    in-RAM ring under a brief critical section, and signals a low-priority writer task. It NEVER
//    touches the SD card on the caller's thread and NEVER allocates.
//  - The in-RAM ring + the writer task stack live in INTERNAL RAM (the writer dereferences buffers
//    inside fwrite/fsync flash-cache-disable windows where any PSRAM access would fault).
//  - event_log_emit() is safe to call before event_log_init() (it just fills the ring; nothing
//    drains to SD until the writer is up).

// Event categories. Keep the set small and debugging-focused.
typedef enum {
    EVL_BOOT = 0,        // power-on / reset (reason + firmware version)
    // The operating mode this boot resolved to. It CANNOT ride on EVL_BOOT: that line is
    // emitted before config.json is parsed, so the mode there would always be the fallback.
    // Carries the stored AND the running mode because they legitimately differ under
    // SmartConnect -- which also makes that discrepancy visible in the log for free.
    EVL_MODE,            // resolved protocol at boot (stored vs running)
    // #98: "ignition", not "engine". The evidence is that the ECU answers, and it answers at
    // key-on with the engine not turning too -- so these can never mean "the crank is spinning".
    EVL_IGNITION_ON,     // poll_log: ECU answering again (bus resume)
    EVL_IGNITION_OFF,    // poll_log: ECU silent -> LISTEN_ONLY quiesce
    // The CRANK actually turning, which IGNITION_ON above deliberately does NOT mean. Own codes
    // rather than EVL_INFO text: this is the same class of fact as the ignition pair and belongs
    // next to it in a scan of the log, and a real code gets the -Wswitch name-table check below.
    // Emitted by poll_log's recording gate off the SAME rpm state the gate itself decides on, so
    // the log can never claim the engine is running while the gate says otherwise.
    EVL_ENGINE_ON,       // poll_log: rpm crossed the running threshold (engine started)
    EVL_ENGINE_OFF,      // poll_log: rpm stopped, or the ECU went silent under a running engine
    EVL_DATALOG_OPEN,    // csv_logger: a logging session/file opened
    EVL_DATALOG_CLOSE,   // csv_logger: a logging session/file closed
    // "UPDATE", not "OTA": the label is what a person reads in their own log, and the acronym
    // says nothing to them. The names are also 12 characters or fewer, which is what the label
    // column pads to (event_log.c) -- see the note there before adding a longer one.
    EVL_UPDATE_START,    // firmware update upload began
    EVL_UPDATE_DONE,     // firmware update written + boot partition switched
    EVL_UPDATE_FAIL,     // firmware update aborted/failed
    // --- NC-Flash / coexistence lifecycle (Task #12). Milestone-only: emitted at start/done/abort
    //     of a flash, fast-read, or host bus session -- NEVER per-block on the TransferData hot path
    //     and NEVER inside an fwrite/fsync flash-cache-disable window (NCFWPROG covers live progress).
    EVL_FLASH_START,     // ncflash_fastwrite: SD-staged fast-write begins (mode, ROM, total blocks)
    EVL_FLASH_OK,        // ncflash_fastwrite: NCFWDONE reached (blocks written, elapsed)
    EVL_FLASH_FAIL,      // ncflash_fastwrite: aborted/failed (where it died: stage + FWSUB/NRC + block)
    EVL_READ_START,      // ncflash_fastread: fast ROM read begins (addr, length)
    EVL_READ_OK,         // ncflash_fastread: fast ROM read completed (bytes, elapsed)
    EVL_HOST_CLAIM,      // host opened the bus-claim window (NC-Flash "cable plugged in")
    EVL_HOST_RELEASE,    // host closed the bus-claim window
    EVL_DATALOG_PARK,    // datalogger parked for a host session (POST /datalog?op=pause)
    EVL_DATALOG_RESUME,  // datalogger resumed after a host session (POST /datalog?op=resume)
    EVL_REAPER_RESUME,   // dead-man reaper auto-resumed datalog (host vanished) -- highest-value line
    EVL_CAN_WAKE,        // wake-on-CAN: verdicts, RXD faults, cooldown (#4)
    // #105: Wi-Fi link milestones from wifi_diag -- association, IP lease, and every disconnect
    // with its reason code. Sparse by design and rate-limited at the source (WD_EVL_REPEAT_MS), so
    // a device stuck in a reconnect loop cannot rotate the rest of this log away. The 1 Hz radio
    // samples wifi_diag also collects are NOT written here; they live only in its RAM aggregates.
    EVL_WIFI,
    EVL_WARN,            // something is degrading but still working (e.g. stack headroom shrinking)
    EVL_INFO,            // generic informational note
    EVL_CODE_MAX
} event_log_code_t;

// SD-mounted predicate injection (optional). event_log is a leaf and cannot call the main-owned
// sdcard_is_mounted(); main registers it here so the writer can skip pointless fopen attempts when
// no card is present. NULL (never set) is fine -- the writer then relies on fopen() failing cleanly.
typedef bool (*event_log_sd_ready_fn_t)(void);
void event_log_set_sd_ready_fn(event_log_sd_ready_fn_t fn);

// Bring up the in-RAM ring + the low-priority SD writer task. Idempotent. Call once early (right
// after the SD mount + restart_tracker_init in app_main). Carries its own RTC_NOINIT crash-guard:
// if a prior boot crashed during event_log SD work, SD persistence is skipped THIS boot (the RAM
// ring still records everything) and self-recovers next boot.
void event_log_init(void);

// True when the RTC crash-guard made event_log_init() skip SD persistence on this boot (the
// in-RAM ring still works). See poll_log_bringup_skipped().
bool event_log_bringup_skipped(void);

// Record one event. Non-blocking, variadic detail (printf-style). Safe from any task and before init.
// Events are fsync'd to SD by the writer task within ~1s of emission, so a reboot a couple of seconds
// later (the planned-restart timer) keeps them; no synchronous flush is needed on the reset path.
//
// !! NEVER CALL THIS FROM THE SYSTEM EVENT TASK (sys_evt), THE esp_timer TASK, OR AN ISR.
// !! Emitting formats the whole line on the CALLER's stack -- ~800 bytes once vsnprintf,
// !! localtime_r and strftime are counted. Those two system tasks have small Kconfig-sized stacks
// !! this firmware does not own, and an ISR has none to spare. Doing it on sys_evt is what
// !! boot-looped a device in v1.19.1 (issue #111): every boot panicked in vApplicationStack-
// !! OverflowHook seconds after the Wi-Fi came up, and no amount of retrying recovered it.
// !! The rule for such callers: capture the bare facts where the event happens (an enum, a few
// !! ints, one short buffer, plus the time -- see event_log_emit_at below), hand them to a task
// !! that owns its own stack, and format there. components/wifi_diag/wifi_diag.c is the worked
// !! example.
// !!
// !! This is ENFORCED, not merely requested: every emit checks its own caller, and a violation
// !! counts up in GET /event_log/status as "bad_ctx". An emit from a banned TASK still goes
// !! through -- the line may be the only record of what went wrong, and a diagnostic must never
// !! brick the device. An emit from an ISR is DROPPED, because the path below takes a lock and a
// !! semaphore in their task forms and cannot legally run there at all.
void event_log_emit(event_log_code_t code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Same as event_log_emit(), but stamped with the time the event HAPPENED instead of the time the
// line is formatted. For deferred emitters: capture tv (gettimeofday) and up_ms
// (esp_timer_get_time()/1000) at the event, format later from a task with a real stack. The
// rendered line is byte-identical in shape to event_log_emit()'s -- same fields, same "unsynced"
// rule for a pre-SNTP clock. Passing tv == NULL means "now" and is exactly event_log_emit(); the
// up_ms argument is ignored in that case.
void event_log_emit_at(event_log_code_t code, const struct timeval *tv, int64_t up_ms,
                       const char *fmt, ...) __attribute__((format(printf, 4, 5)));

// ---- Debug-detail gate (#98) -----------------------------------------------------------------
// This event log has no severity levels: everything emitted lands in the ring, on the SD card and
// in the web UI's Event Log card. A few lines are pure diagnostics that a normal user should not
// have to read, so they go through EVENT_LOG_DEBUG(), which records ONLY while the stored "debug"
// config flag is on. Gated off it records NOTHING ANYWHERE -- there is deliberately no serial
// fallback, because this build compiles ESP_LOGD out (CONFIG_LOG_MAXIMUM_LEVEL=INFO) and the board
// has no serial console a PC can read anyway (its USB-C port is a USB host at runtime).
//
// !! NEVER route a safety or forensic line through EVENT_LOG_DEBUG(). A gated-off line is gone for
// !! good, and turning debug on later cannot recover what already happened. "entering sleep",
// !! "resume FAILED", the sleep postpones, and the OTA/flash lifecycle are the only post-hoc
// !! evidence this device has after a bad night in a car. They stay always-visible.
// Use it for detail that is merely nice to have, and prefer splitting a line (plain always,
// numbers on debug) over hiding the event itself.
//
// A MACRO, not a function, on purpose: it must not EVALUATE its arguments when the gate is shut.
// The one call site passes heap_caps_get_largest_free_block(), which walks the heap free lists
// under the heap lock -- a function call would pay for that on every wake forever.
// Forwards to event_log_emit(), so printf format checking still applies.
//
// The flag is pushed in from main rather than read from config_server: this component is a leaf
// and main depends on components, not the reverse. Reported by GET /event_log/status as "debug".
void event_log_set_debug(bool on);
bool event_log_debug_enabled(void);

#define EVENT_LOG_DEBUG(code, ...) \
    do { if (event_log_debug_enabled()) event_log_emit((code), __VA_ARGS__); } while (0)

// Register the GET /event_log* retrieval endpoint on the running httpd server. Idempotent. Must be
// called before the catch-all wildcard handler. Routes:
//   /event_log            -> the SD log file (chunked), or the RAM ring if no card
//   /event_log/ram        -> the in-RAM ring (always works)
//   /event_log/status     -> JSON {sd, file_bytes, rotations, ring_count, dropped, ...}
esp_err_t event_log_register_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
