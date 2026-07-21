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

#ifndef __AUTO_PID_H__
#define __AUTO_PID_H__

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>
#include <time.h>

#define AUTOPID_BUFFER_SIZE (1024*4)
#define QUEUE_SIZE 10

// Safety caps for user-configurable destinations (prevents stack/heap abuse via config JSON)
#define AUTOPID_MAX_DESTINATIONS 6

/* Sweep-divisor cap (issue #29). Bounds polllog_prepare_schedule()'s two stack arrays at
 * 65 bytes each and keeps every divisor inside the uint8_t counter. Enforced in BOTH the
 * parser (json_item_to_sample_every) and polllog_prepare_schedule(); the latter is the
 * authoritative enforcement point because it INDEXES arrays with the value while running
 * on the sole-TWAI-owner poll task. The UI validator must use the same 64. */
#define AUTOPID_MAX_SAMPLE_EVERY 64

typedef struct {
    uint8_t data[AUTOPID_BUFFER_SIZE];
    uint32_t length;
    uint8_t* priority_data;
    uint8_t  priority_data_len;
} response_t;

typedef enum
{
    SENSOR = 0,
    BINARY_SENSOR = 1,
} sensor_type_t;

////////////////

typedef enum
{
    PID_STD = 0,
    PID_CUSTOM = 1,
    PID_SPECIFIC = 2,
    PID_MAX
}pid_type_t;


typedef struct 
{
    char *name;
    char *expression;
    char *unit;
    char *class;
    bool enabled;
    uint32_t period; 
    float min;
    float max;
    sensor_type_t sensor_type;
    int64_t timer;
    float value;
    bool failed;
}parameter_t;

typedef struct 
{
    char* cmd;
    char* init;
    uint32_t period; 
    parameter_t *parameters;
    uint32_t parameters_count;
    pid_type_t pid_type;
    char* rxheader;
    bool enabled;
    /* Sweep-divisor scheduling (issue #29): poll this PID on every Nth poll_log sweep.
     * 0 or 1 == every sweep -- the default and today's shipped behaviour. Parsed from
     * "SampleEvery" (auto_pid.json pids/std_pids) or "sample_every" (car_data.json pids).
     * Inert outside the POLL_LOG protocol (Legacy AutoPID and fast_log have no sweep). */
    uint8_t sample_every;
    /* Executed sweeps still to skip before this PID is due. Seeded at config load by
     * polllog_prepare_schedule() with this PID's phase offset; decremented once per
     * EXECUTED, GATED sweep. Owned exclusively by the poll task, never persisted, always
     * re-derived after a live hot-reload (poll_log.c:466-489). */
    uint8_t sample_ctr;
}pid_data_t;

// CAN filter configuration (broadcast frames parsing)
// Each filter refers to one CAN frame ID that may yield multiple parameters.
typedef struct
{
    uint32_t frame_id;
    bool is_extended; // inferred: frame_id > 0x7FF
    // True if this filter came from car_data.json (vehicle profile);
    // false if it came from auto_pid.json (custom filters).
    bool is_vehicle_specific;
    parameter_t *parameters;
    uint32_t parameters_count;
} can_filter_t;

typedef struct 
{
    pid_data_t *pids;
    uint32_t pid_count;
    // CAN filters (broadcast frames)
    can_filter_t *can_filters;
    uint32_t can_filters_count;
    // Calculated channels (Task #17): math over OTHER decoded channel values (source "CALC").
    // Reuses parameter_t (name/expression/unit/value/enabled/min/max); expression references
    // channel NAMES, not raw CAN bytes -- evaluated by autopid_eval_calculated_channels().
    parameter_t *calculated;
    uint32_t calculated_count;
    char* custom_init;
    char* standard_init;
    char* specific_init;
    char* selected_car_model;
    bool pid_std_en;
    bool pid_custom_en;
    bool pid_specific_en;
    // When enabled, pause Automate/AutoPID when battery voltage is below configured sleep voltage.
    // Stored in auto_pid.json as: disable_on_sleep_voltage = "enable"/"disable".
    bool disable_on_sleep_voltage;
    // Alternative low-voltage mode: when battery voltage is below configured sleep voltage,
    // disable PID requests (polling) but keep CAN filter monitoring active.
    // Stored in auto_pid.json as: disable_on_sleep_voltage = "disable_pid_requests".
    bool disable_pid_requests_on_sleep_voltage;

    // Alternative voltage mode: disable PID requests (polling) when battery voltage is below
    // a configurable threshold (separate from Power Saving -> Sleep Voltage).
    // CAN filter monitoring remains active.
    // Stored in auto_pid.json as: disable_on_sleep_voltage = "automate_threshold".
    bool disable_pid_requests_on_automate_threshold;

    // Voltage threshold used when disable_pid_requests_on_automate_threshold is enabled.
    // Stored in auto_pid.json as: pid_polling_min_voltage = <number>.
    float pid_polling_min_voltage;
    // When enabled, validate that each PID request's response matches the request (service + PID bytes)
    // using the command string (cmd_str) provided by the ELM command runner.
    bool pid_validation_en;
    char* std_ecu_protocol;
    char* vehicle_model;
    uint32_t cycle;     //To be removed when std pid gets its own period
    time_t last_successful_pid_time;  // Timestamp in seconds since epoch of last successful PID response
    // NB: no per-config mutex -- serialization is via the file-static s_autopid_mutex in
    // autopid.c (created once, never destroyed, so it stays valid across live table swaps).
} autopid_config_t;

typedef struct 
{
    char *json_str;              // Pointer to a dynamically allocated string
    SemaphoreHandle_t mutex; // Mutex to protect access to the data
} autopid_data_t;

void autopid_parser(char *str, uint32_t len, QueueHandle_t *q, char* cmd_str);
void autopid_init(char* id);

// FAST_LOG support (Task #18): parse auto_pid.json into the module config WITHOUT starting
// the AutoPID task or any ELM polling. Creates autopid_config + its mutex and registers the
// wide-CSV column provider, so the native-TWAI fast datalogger (components/fast_log) can
// read can_filters[]/pids[] and reuse autopid_lock()/the column provider. Returns the parsed
// config (NULL on failure). Idempotent: returns the existing config if already loaded.
// Unlike autopid_init() it does NOT require pid_count>0 (broadcast-only configs are valid).
autopid_config_t* autopid_load_config_only(void);
char *autopid_data_read(void);
bool autopid_get_ecu_status(void);
char* autopid_get_config(void);
esp_err_t autopid_find_standard_pid(uint8_t protocol, char *available_pids, uint32_t available_pids_size) ;

// Protocol tracking helpers used by AutoPID parsing and related modules.
esp_err_t autopid_set_protocol_number(int32_t protocol_value);
esp_err_t autopid_get_protocol_number(int32_t *protocol_value);

char *autopid_get_value_by_name(char* name);
void autopid_app_reset_timer(void);

// Shared lock for ELM327 access.
// The AutoPID task uses this mutex to serialize access to the ELM327 interface.
// Other modules (e.g. AutoPID HTTP test endpoint) should take this lock to
// prevent interleaved commands/responses.
bool autopid_lock(uint32_t timeout_ms);
void autopid_unlock(void);

// Calculated channels (Task #17): evaluate every configured calculated channel from the latest
// decoded source-channel values and record each as a wide-CSV column (source "CALC"). Called once
// per decode sweep by the native-TWAI loggers (poll_log/fast_log). Takes autopid_lock() internally
// (the caller must NOT already hold it); does no bus/SD/flash I/O; a no-op when no calculated
// channels are configured.
void autopid_eval_calculated_channels(void);

// --- Live PID-table hot-swap (issue #39) --------------------------------------
// Deep-free a table produced by load_autopid_config() (mirrors its allocation set;
// never touches the hoisted config mutex). Public so the reload path can retire the
// old table after the swap.
void autopid_config_deep_free(autopid_config_t *c);

// Returned by autopid_reload_config() when a CSV trip opened between the poll task's
// safe-point check and the swap (issue #43 P1): NOT a rejection -- the parsed table is
// fine but swapping under a just-opened trip would mismatch its frozen header. The
// caller must re-arm s_reload_requested so the swap retries once the trip closes.
// A distinct non-NULL sentinel (never a real table); the caller must not dereference it.
#define AUTOPID_RELOAD_DEFERRED ((autopid_config_t *)-1)

// Re-parse auto_pid.json and atomically swap it in for the live autopid_config.
// MUST be called ONLY from the poll task at its safe point (holds no table pointer).
// Validates the new table (rejects only enabled pids with no cmd / inconsistent
// parameter arrays) and keeps the old table on any failure. Returns the new table on
// success, NULL if the reload was rejected (caller keeps running the old table), or
// AUTOPID_RELOAD_DEFERRED if a CSV trip opened mid-reload (caller re-arms and retries).
autopid_config_t *autopid_reload_config(void);

// Serialize auto_pid.json file writes against the reload's count+parse (P2 TOCTOU
// guard). store_auto_data_handler holds this around its fwrite; the reload holds it
// around load_autopid_config(). No-op if the lock was never created (non-autopid modes).
void autopid_file_lock(void);
void autopid_file_unlock(void);
#endif
