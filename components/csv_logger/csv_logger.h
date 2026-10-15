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

#ifndef __CSV_LOGGER_H__
#define __CSV_LOGGER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How long the ignition must read OFF before a trip is closed. Shared, not copied: poll_log's
 * recording gate debounces on the same value, so the poller and the writer agree on the moment the
 * engine stopped instead of drifting apart. */
#define CSV_LOGGER_IGN_OFF_DEBOUNCE_MS  3000

#define CSV_LOGGER_DIR              "/sdcard/logs"
#define CSV_LOGGER_NAME_MAX         48
#define CSV_LOGGER_UNIT_MAX         16
#define CSV_LOGGER_SOURCE_MAX       8

/**
 * @brief Start the CSV datalogger.
 *
 * Creates the record queue and writer task. Safe to call once at startup;
 * if the logger is disabled in config the caller should simply not call this.
 * Records are written to /sdcard/logs/<name>.csv, one file per ignition cycle.
 *
 * @return ESP_OK on success
 */
esp_err_t csv_logger_init(void);

/**
 * @brief Runtime manual Start/Stop for the CSV logger (web button, authoritative).
 *
 * Like the CAN Monitor button: START forces logging ON and STOP forces it OFF, both
 * overriding the ignition gate. Runtime-only, NOT persisted -- a reboot resets to AUTO
 * (follow ignition) under the csv_log config boot gate.
 *
 * enable=true:  force logging ON regardless of ignition; lazily creates the writer task
 *   (csv_logger_init()) if csv_log was disabled at boot. MUST be called from a task at
 *   priority > 4 (the httpd handler at prio 5 qualifies) so the on-demand init cannot hit
 *   the boot publish-race. Returns the csv_logger_init() error on a failed start (e.g. OOM),
 *   leaving the mode OFF. Note: a session only opens once a record arrives, so logging is
 *   effective only while AutoPID records are flowing.
 * enable=false: force logging OFF for the rest of THIS TRIP even if ignition reads on; the
 *   writer closes the session on its next pass and stays alive (never deleted). Cleared back
 *   to AUTO on the next ignition-off edge, so the following key-on records normally -- Stop
 *   is per-trip, not "disable auto-logging until someone reboots". FORCE_ON is NOT cleared
 *   that way: bench work needs it to survive a voltage flapping across the threshold.
 *
 * @return ESP_OK, or the csv_logger_init() error on a failed on-demand start.
 */
esp_err_t csv_logger_set_manual_override(bool enable);

/**
 * @brief Column provider for the WIDE (Tactrix-style) CSV format (Task #11).
 *
 * Registered by the AutoPID module at init so the CSV logger can enumerate the set of
 * logged channels (one column each) WITHOUT a build-time dependency on autopid. autopid
 * already REQUIRES csv_logger; routing the enumeration through a function pointer keeps
 * that dependency one-way (no circular CMake REQUIRES).
 *
 * The provider fills caller-supplied fixed-stride arrays and returns the column count, or
 * -1 if the config/lock is not ready (the writer retries briefly, then falls back to LONG).
 * Counting mode: names==NULL returns an UPPER BOUND on the count (no writes) so the caller
 * can size buffers before the fill pass. The provider takes its own lock BRIEFLY and does
 * NO SD/flash/PSRAM-heavy work while holding it.
 *
 * Array dims MUST equal CSV_LOGGER_NAME_MAX / CSV_LOGGER_UNIT_MAX / CSV_LOGGER_SOURCE_MAX.
 * The (source,name) pair is the column key: the producer emits the same name under
 * different source tags (e.g. "PID" vs "CANFLT"), so source disambiguates two channels.
 */
typedef int (*csv_column_provider_t)(char (*names)[CSV_LOGGER_NAME_MAX],
                                     char (*units)[CSV_LOGGER_UNIT_MAX],
                                     char (*sources)[CSV_LOGGER_SOURCE_MAX],
                                     int max_cols);

/**
 * @brief Register the wide-CSV column provider. Call once at boot (before logging starts).
 */
void csv_logger_set_column_provider(csv_column_provider_t provider);

/**
 * @brief Engine-running predicate for the "Require engine running" CSV gate.
 *
 * Returns true while logging is warranted. poll_log registers poll_log_gate_open() here so the
 * logger can stop on engine-off without a circular component dependency (poll_log already depends
 * on csv_logger). NOT poll_log_ignition_on(): that one means "the ECU answers", which is also
 * true of a parked car at key-on. Registered once at boot; when no provider is set the gate
 * degrades to the voltage ignition gate only. Must be cheap and lock-free (called from the writer
 * task).
 */
typedef bool (*csv_engine_state_fn_t)(void);
void csv_logger_set_engine_state_fn(csv_engine_state_fn_t fn);

/**
 * @brief "The ECU stopped answering" predicate, one of the signs that end a trip (#109).
 *
 * Returns true only while the ECU is known to be silent. A true here clears a manual Stop, so
 * the provider must FAIL CLOSED (false) whenever it cannot tell -- a false only means the Stop
 * waits for the voltage or sleep sign instead. poll_log registers poll_log_ecu_silent(); same
 * no-circular-dependency registration pattern as the engine-state predicate. Must be cheap and
 * lock-free (called from the writer task and the sleep teardown).
 */
typedef bool (*csv_ecu_silent_fn_t)(void);
void csv_logger_set_ecu_silent_fn(csv_ecu_silent_fn_t fn);

/**
 * @brief Measured sample-rate provider for the "Auto" fixed-rate grid (issue #23).
 *
 * Returns the current fastest meaningful logging rate in Hz (poll_log registers its measured
 * full-sweep EMA), or 0 when unknown -- the writer then falls back to its default rate. Same
 * no-circular-dependency registration pattern as the engine-state predicate. Must be cheap and
 * lock-free (called from the writer task on every grid tick).
 */
typedef float (*csv_rate_fn_t)(void);
void csv_logger_set_rate_fn(csv_rate_fn_t fn);

/**
 * @brief Live-stream hook: a copy of every line the writer puts in the trip file (issue #3).
 *
 * main/datalog_stream.c registers one to feed the 35002 live tail; the logger cannot depend on
 * main, hence the function pointer (same one-way pattern as the providers above). Called ONLY
 * from the writer task, so the hook sees events in file order and needs no lock of its own.
 * It must never block: it runs between SD writes, and a slow hook is a slow trip log.
 *
 *   OPEN  : a trip file just opened (also after a size rotation). file = full path, cols = the
 *           channel-column count, line/len = the whole header line incl. '\n', or NULL/0 when
 *           the header did not fit the row buffer.
 *   ROW   : line/len = the row just written, incl. '\n'.
 *   CLOSE : the trip file just closed (trip end, stop, SD removed, rotation, write error).
 *   POLL  : once per writer-loop pass (at least every 250 ms) so the hook can answer a newly
 *           connected client even when no trip is open. No data.
 */
typedef enum
{
    CSV_STREAM_EV_OPEN = 0,
    CSV_STREAM_EV_ROW,
    CSV_STREAM_EV_CLOSE,
    CSV_STREAM_EV_POLL,
} csv_stream_event_t;

typedef void (*csv_stream_hook_t)(csv_stream_event_t ev, const char *file, int cols,
                                  const char *line, size_t len);

/* Register the live-stream hook. Call before csv_logger_start_at_boot() so the first trip's
 * OPEN is seen; a hook registered later only learns about the NEXT trip. */
void csv_logger_set_stream_hook(csv_stream_hook_t hook);

/* True once the writer task exists (csv_log enabled at boot, or a manual Start). */
bool csv_logger_running(void);

/**
 * @brief Start the CSV datalogger at boot, crash-guard gated.
 *
 * Call this at boot instead of csv_logger_init(). It brings the writer up INLINE -- no task
 * is spawned on the normal path and there is NO start delay, so the first file opens when
 * the ECU actually answers (~3-5 s) rather than when a timer says so.
 *
 * Safe to call from app_main at priority 1 even though the writer runs at 4: the writer
 * cannot observe a NULL queue because csv_logger_init() publishes the queue BEFORE
 * creating the task. (That publish race, not any settling requirement, was the historical
 * "boot crash" -- which is why the priority note on csv_logger_set_manual_override() below
 * no longer applies here.)
 *
 * If the RTC guard shows the previous attempt did not survive its first
 * CSV_GUARD_STABLE_US, bring-up is skipped, with one delayed retry before it gives up for
 * the boot -- see csv_bringup_logic.h for the chain and its boot-loop bound.
 */
void csv_logger_start_at_boot(void);

/* True when the RTC crash-guard made csv_logger_start_at_boot() skip CSV auto-start on this boot.
 * See poll_log_bringup_skipped(). */
bool csv_logger_bringup_skipped(void);

/* Tell the writer task to close any open session (true) or that it may log again (false).
 * The sleep teardown sets it and the resume clears it, so a session never spans a sleep: a wake
 * resumes in place, so an open file would otherwise come back with its timers jumped by hours.
 * Overrides even a manual FORCE_ON. Setting it also clears a manual Stop, because a sleep ends
 * the trip (#109). */
void csv_logger_set_sleep_requested(bool sleeping);

/**
 * @brief Queue one decoded parameter sample for CSV logging.
 *
 * Non-blocking: called from the AutoPID hot path, so it never waits. If the
 * logger is not running or the queue is full the sample is counted as dropped
 * and the call returns immediately.
 *
 * @param name   Parameter name (e.g. "EngineRPM")
 * @param value  Decoded value
 * @param unit   Unit string, may be NULL
 * @param source Origin tag: "CANFLT" (broadcast), "PID" or "STD" (polled)
 */
void csv_logger_record(const char *name, float value, const char *unit, const char *source);

/**
 * @brief Logger status snapshot as a cJSON-printed string (caller frees), or NULL.
 */
char *csv_logger_get_status_json(void);

/**
 * @brief True if abspath is the CSV file the writer task currently holds open.
 *
 * Lets the SD file manager refuse to delete/rename the log being written.
 */
bool csv_logger_is_active_file(const char *abspath);

/**
 * @brief True while a CSV log session is open (file on SD, rows being accepted).
 *
 * Cross-task snapshot of the writer-task-owned session flag; drives the blue
 * LED activity indicator (issue #19).
 */
bool csv_logger_session_active(void);

/* HTTP endpoints (register in config_server):
 *   GET  /csv_status            -> logger status JSON (running, sd_mounted, session_active, rows_written...)
 *   GET  /csv_list              -> {"files":[{"name":..,"size":..}, ...]} of the SD-card CSV logs
 *   GET  /download_csv?file=NAME -> streams that CSV file as a download
 */
extern const httpd_uri_t csv_status_uri;
extern const httpd_uri_t csv_list_uri;
extern const httpd_uri_t csv_download_uri;
extern const httpd_uri_t csv_control_uri;     /* POST /csv_logger?op=start|stop|mark */
extern const httpd_uri_t datalog_control_uri; /* POST /datalog?op=pause|resume|bus_claim|bus_release|keepalive */
extern const httpd_uri_t datalog_status_uri;  /* GET  /datalog -> live coexistence state */

/**
 * @brief Restore the pre-pause manual mode after a park lease has been released/reaped
 *        (dead-man's-switch, WICAN_DEADMAN_AUTORESUME.md).
 *
 * Called by exactly one winner per park lease: the REST op=resume handler (after a successful
 * token-matched can_park_lease_release) OR the firmware dead-man reaper (after a successful
 * token+deadline-matched can_park_lease_reap). Because both lower the park flag atomically under
 * the can.c spinlock, only one reaches here for a given pause -- that mutual exclusion is what
 * makes this lock-free mode-restore safe. Touches ONLY the csv manual mode, never a CAN bit.
 */
void datalog_restore_mode(void);

#ifdef __cplusplus
}
#endif

#endif
