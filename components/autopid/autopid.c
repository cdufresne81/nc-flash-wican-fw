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

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include <ctype.h>
#include <string.h>
#include "autopid.h"
#include "cJSON.h"
#include "autopid.h"
#include <math.h>
#include "obd2_standard_pids.h"
#include <float.h>
#include "csv_logger.h"
#include "autopid_config.h"
#include "esp_heap_caps.h"
#include <stdlib.h>   /* strtod for the calculated-channel evaluator (Task #17) */

// #define TAG __func__
#define TAG "AUTO_PID"
// #define TAG __func__
#define TAG "AUTO_PID"

#define ECU_CONNECTED_BIT BIT0
static EventGroupHandle_t xautopid_event_group = NULL;
static autopid_config_t *autopid_config = NULL;
// Config mutex hoisted out of the swappable autopid_config struct (issue #39): a live
// PID-table hot-swap frees the old struct, so a per-struct mutex handle (the former
// autopid_config->mutex) was a use-after-free once locked cross-task after a swap.
// This file-static handle is created once and never destroyed, so it stays valid
// across swaps and is the single serialization point for the config pointer.
static SemaphoreHandle_t s_autopid_mutex = NULL;
// Serializes auto_pid.json file writes (store_auto_data_handler) against the live
// reload's count+parse (P2 TOCTOU guard, issue #39). Created once at config load.
static SemaphoreHandle_t s_autopid_file_lock = NULL;
static autopid_data_t autopid_data = {.json_str = NULL, .mutex = NULL};

bool autopid_lock(uint32_t timeout_ms)
{
    if (!autopid_config || !s_autopid_mutex)
        return false;

    TickType_t to = pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTake(s_autopid_mutex, to) == pdTRUE);
}

void autopid_unlock(void)
{
    if (!autopid_config || !s_autopid_mutex)
        return;

    xSemaphoreGive(s_autopid_mutex);
}

// --- Live per-row Test under POLL_LOG (issue #41) -----------------------------
// Dependency-inversion registry (mirrors csv_logger_set_rate_fn): poll_log registers its
// in-band one-shot executor here; the /autopid/test_pid and /autopid/test_can_filter
// handlers dispatch through autopid_live_test(), so autopid never #includes fast_log.
static autopid_live_test_fn_t s_live_test_fn = NULL;   // single-writer, set once at poll_log_init

void autopid_set_live_test_fn(autopid_live_test_fn_t fn)
{
    s_live_test_fn = fn;
}

autopid_live_test_status_t autopid_live_test(const autopid_live_test_req_t *req,
                                             autopid_live_test_res_t *res, uint32_t timeout_ms)
{
    if (!s_live_test_fn)
    {
        // No executor: non-POLL_LOG mode, or poll_log_init() returned early (0-PID broadcast-only
        // config, crash-guard, bring-up failure) before registering. The handlers key their JSON
        // off res->status, NOT this return value, so res MUST be filled -- otherwise they switch on
        // an uninitialized stack struct and can over-read res->raw (issue #41 review).
        if (res)
        {
            memset(res, 0, sizeof(*res));
            res->status = AUTOPID_TEST_INACTIVE;
        }
        return AUTOPID_TEST_INACTIVE;   // POLL_LOG not running -> handler emits "set protocol"
    }
    return s_live_test_fn(req, res, timeout_ms);
}

// --- Live PID-table hot-swap (issue #39) --------------------------------------
// File lock (P2): serialize auto_pid.json writes against the reload's count+parse so a
// write landing between load_autopid_config()'s count and parse can't make parse iterate
// a larger array than was allocated (heap overflow). No-op until created at config load.
void autopid_file_lock(void)
{
    if (s_autopid_file_lock)
        xSemaphoreTake(s_autopid_file_lock, portMAX_DELAY);
}

bool autopid_file_trylock(uint32_t wait_ms)
{
    if (!s_autopid_file_lock)
        return true;   // same no-op-until-created rule as autopid_file_lock()
    const TickType_t ticks = (wait_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
    return xSemaphoreTake(s_autopid_file_lock, ticks) == pdTRUE;
}

void autopid_file_unlock(void)
{
    if (s_autopid_file_lock)
        xSemaphoreGive(s_autopid_file_lock);
}

autopid_config_t *autopid_reload_config(void)
{
    // 1. Re-parse auto_pid.json OFF the config lock (SD/flash I/O), serialized against
    //    the handler's file write so we never parse a torn size (P2).
    autopid_file_lock();
    autopid_config_t *new_cfg = load_autopid_config();
    autopid_file_unlock();
    if (!new_cfg)
    {
        ESP_LOGW(TAG, "reload: load_autopid_config returned NULL, keeping old table");
        return NULL;
    }

    // 2. Known-good validation. cmd==NULL is a NORMAL, poll-loop-tolerated state -- not only
    //    for disabled/std/tail pids but for an ENABLED pid whose name doesn't resolve to a
    //    command (polllog_poll_one returns on !pid->cmd regardless of enabled). Rejecting on
    //    enabled+cmd==NULL would refuse a table a reboot loads fine, breaking reboot-equivalence
    //    and turning the handler's "live" reply into a lie. So validate only the shape that is
    //    a real hazard: a parameter array whose count disagrees with its (NULL) base -> an
    //    out-of-bounds read in the poll loop. On reject, keep the old table (never brick).
    if (new_cfg->pids)
    {
        for (uint32_t i = 0; i < new_cfg->pid_count; i++)
        {
            pid_data_t *p = &new_cfg->pids[i];
            if (p->parameters_count > 0 && p->parameters == NULL)
            {
                ESP_LOGW(TAG, "reload: pid %lu parameter array inconsistent, rejecting reload", (unsigned long)i);
                autopid_config_deep_free(new_cfg);
                return NULL;
            }
        }
    }

    // 3. Swap under the config lock (single aligned pointer store). Take s_autopid_mutex
    //    directly with portMAX_DELAY (a true infinite wait) -- autopid_lock() would run it
    //    through pdMS_TO_TICKS and overflow it to a finite timeout. Matches the file's other
    //    swap-critical sites. autopid_config is non-NULL here (the old table is still live).
    if (xSemaphoreTake(s_autopid_mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGW(TAG, "reload: could not take config lock, rejecting reload");
        autopid_config_deep_free(new_cfg);
        return NULL;
    }

    // Trip-open vs reload race (issue #43 P1): the safe-point check
    // (!csv_logger_session_active()) can pass, then this function spends ~100 ms in
    // load_autopid_config() while the CSV writer opens a trip -- its header is frozen
    // from the OLD table but records would then come from the NEW one for the whole
    // trip (column mismatch). Re-check UNDER s_autopid_mutex: the column provider
    // (autopid_collect_log_columns) takes the same mutex to read the table, so if it
    // already ran, its read strictly precedes this re-check. If a session is now open,
    // defer -- don't swap; the caller re-arms and retries once the trip closes. This
    // shrinks the exposure from the whole parse to the writer's file-open instant.
    if (csv_logger_session_active())
    {
        xSemaphoreGive(s_autopid_mutex);
        ESP_LOGI(TAG, "reload: CSV trip opened mid-parse, deferring swap until it closes");
        autopid_config_deep_free(new_cfg);
        return AUTOPID_RELOAD_DEFERRED;
    }

    autopid_config_t *old_cfg = autopid_config;
    autopid_config = new_cfg;
    xSemaphoreGive(s_autopid_mutex);

    // 4. Free the old table AFTER the global was republished under the lock and every
    //    reader observes the new base (see the UAF-free proof in docs/goal-live-reconfigure.md).
    autopid_config_deep_free(old_cfg);
    return new_cfg;
}

// Column provider for the wide CSV logger (Task #11). Registered via
// csv_logger_set_column_provider() in autopid_load_config_only so csv_logger has no build-time
// dependency on autopid (autopid already REQUIRES csv_logger -> keeps the dep one-way).
//
// Mirrors the PRODUCER enable-gates exactly (NOT the JSON-export walk), so the wide header
// lists precisely the channels that csv_logger_record will emit at runtime -- no phantom
// all-empty columns, no missing channels. Source tags MUST match the producer call sites:
//   PIDs:        STD (pid_type==PID_STD) / PID (CUSTOM,SPECIFIC)   [polllog_poll_one]
//   CAN filters: CANFLT                                            [polllog_decode_broadcast]
//   Calculated:  CALC                                              [autopid_eval_calculated_channels]
//
// names==NULL -> count-only (returns an upper bound, no writes, no dedup). Otherwise fills
// the caller arrays with the deduped set, keyed on (source,name), and returns the count.
// Returns -1 if the config/mutex isn't ready. Takes autopid_lock for a BRIEF, bounded time
// (100 ms): the calling writer task is prio 4, so we must never block long here. Does NO
// SD/flash work locked.
static int autopid_collect_log_columns(char (*names)[CSV_LOGGER_NAME_MAX],
                                       char (*units)[CSV_LOGGER_UNIT_MAX],
                                       char (*sources)[CSV_LOGGER_SOURCE_MAX],
                                       int max_cols)
{
    if (!autopid_config || !s_autopid_mutex)
        return -1;
    if (!autopid_lock(100))
        return -1;

    bool count_only = (names == NULL);
    int count = 0;

    // ---- Configured PIDs ----
    for (uint32_t i = 0; i < autopid_config->pid_count && count < max_cols; i++)
    {
        pid_data_t *pid = &autopid_config->pids[i];
        if ((pid->pid_type == PID_STD && !autopid_config->pid_std_en) ||
            (pid->pid_type == PID_CUSTOM && !autopid_config->pid_custom_en) ||
            (pid->pid_type == PID_SPECIFIC && !autopid_config->pid_specific_en))
            continue;
        if (!pid->enabled)
            continue;
        const char *src = (pid->pid_type == PID_STD) ? "STD" : "PID";
        for (uint32_t j = 0; j < pid->parameters_count && count < max_cols; j++)
        {
            parameter_t *p = &pid->parameters[j];
            if (!p->enabled || !p->name || p->name[0] == '\0')
                continue;
            if (!count_only)
            {
                bool dup = false;
                for (int k = 0; k < count; k++)
                    if (strcmp(names[k], p->name) == 0 && strcmp(sources[k], src) == 0)
                    { dup = true; break; }
                if (dup) continue;
                strlcpy(names[count], p->name, CSV_LOGGER_NAME_MAX);
                strlcpy(units[count], p->unit ? p->unit : "", CSV_LOGGER_UNIT_MAX);
                strlcpy(sources[count], src, CSV_LOGGER_SOURCE_MAX);
            }
            count++;
        }
    }

    // ---- CAN-filter (broadcast) params ----
    for (uint32_t fi = 0; fi < autopid_config->can_filters_count && count < max_cols; fi++)
    {
        can_filter_t *f = &autopid_config->can_filters[fi];
        if (f->is_vehicle_specific && !autopid_config->pid_specific_en)
            continue;
        for (uint32_t pi = 0; pi < f->parameters_count && count < max_cols; pi++)
        {
            parameter_t *p = &f->parameters[pi];
            if (!p->enabled || !p->expression || !p->name || p->name[0] == '\0')
                continue;
            if (!count_only)
            {
                bool dup = false;
                for (int k = 0; k < count; k++)
                    if (strcmp(names[k], p->name) == 0 && strcmp(sources[k], "CANFLT") == 0)
                    { dup = true; break; }
                if (dup) continue;
                strlcpy(names[count], p->name, CSV_LOGGER_NAME_MAX);
                strlcpy(units[count], p->unit ? p->unit : "", CSV_LOGGER_UNIT_MAX);
                strlcpy(sources[count], "CANFLT", CSV_LOGGER_SOURCE_MAX);
            }
            count++;
        }
    }

    // ---- Calculated channels (Task #17, source "CALC") ----
    // Listed AFTER pids and can_filters so column order is PID/STD, then CANFLT, then CALC.
    // Mirrors the producer gate in autopid_eval_calculated_channels(): enabled + named + has expr.
    for (uint32_t ci = 0; ci < autopid_config->calculated_count && count < max_cols; ci++)
    {
        parameter_t *p = &autopid_config->calculated[ci];
        if (!p->enabled || !p->name || p->name[0] == '\0' ||
            !p->expression || p->expression[0] == '\0')
            continue;
        if (!count_only)
        {
            bool dup = false;
            for (int k = 0; k < count; k++)
                if (strcmp(names[k], p->name) == 0 && strcmp(sources[k], "CALC") == 0)
                { dup = true; break; }
            if (dup) continue;
            strlcpy(names[count], p->name, CSV_LOGGER_NAME_MAX);
            strlcpy(units[count], p->unit ? p->unit : "", CSV_LOGGER_UNIT_MAX);
            strlcpy(sources[count], "CALC", CSV_LOGGER_SOURCE_MAX);
        }
        count++;
    }

    autopid_unlock();
    if (!count_only && count >= max_cols)
        ESP_LOGW(TAG, "Wide CSV column cap (%d) reached - some channels omitted", max_cols);
    return count;
}

/* ===== Calculated channels (Task #17) =====================================================
 * Self-contained recursive-descent evaluator over decoded CHANNEL NAMES, e.g.
 *   "MAP - BARO"   "EQ_RATIO * 14.64"   "SPARKADV - KNOCKR"
 * Deliberately SEPARATE from expression_parser.c's evaluate_expression(): that one is
 * single-char-sigil / raw-CAN-byte (B0..B7, S<n>, [Bx:By], V) and runs live on the car for
 * PID/broadcast decode -- it cannot lex multi-letter channel names and must not be touched.
 *
 * Grammar (left-assoc; '*' '/' bind tighter than '+' '-'; unary +/- supported):
 *   expr    := term (('+'|'-') term)*
 *   term    := primary (('*'|'/') primary)*
 *   primary := ('+'|'-') primary | '(' expr ')' | number | identifier
 * Identifiers are bare names [A-Za-z_][A-Za-z0-9_]* resolved to a source channel's latest
 * decoded value. An unknown or not-yet-decoded (FLT_MAX sentinel) input fails the whole
 * expression so the calc column simply stays empty (LOCF) until its inputs exist. Allocation-
 * free; bounded recursion; reads source values under the caller's autopid_lock.
 */
#define CALC_MAX_DEPTH 32

typedef struct
{
    const char *s;            // parse cursor
    autopid_config_t *cfg;    // source-channel universe (read under autopid_lock)
    int depth;                // recursion guard
    bool ok;                  // cleared on any parse/resolve error
} calc_ctx_t;

// Resolve a length-delimited channel name to its latest decoded value. Scans polled PIDs then
// CAN filters; returns the first FINITE (already-decoded) match. Unknown or all-stale (FLT_MAX)
// -> false, which fails the calc for this sweep.
static bool calc_resolve_name(autopid_config_t *cfg, const char *name, size_t len, double *out)
{
    if (!cfg || !name || len == 0)
        return false;
    for (uint32_t i = 0; i < cfg->pid_count; i++)
    {
        pid_data_t *pid = &cfg->pids[i];
        for (uint32_t j = 0; j < pid->parameters_count; j++)
        {
            parameter_t *p = &pid->parameters[j];
            if (p->name && strlen(p->name) == len && strncmp(p->name, name, len) == 0 &&
                p->value != FLT_MAX && isfinite(p->value))
            {
                *out = (double)p->value;
                return true;
            }
        }
    }
    for (uint32_t fi = 0; fi < cfg->can_filters_count; fi++)
    {
        can_filter_t *f = &cfg->can_filters[fi];
        for (uint32_t pi = 0; pi < f->parameters_count; pi++)
        {
            parameter_t *p = &f->parameters[pi];
            if (p->name && strlen(p->name) == len && strncmp(p->name, name, len) == 0 &&
                p->value != FLT_MAX && isfinite(p->value))
            {
                *out = (double)p->value;
                return true;
            }
        }
    }
    return false;
}

static void calc_skipws(calc_ctx_t *c)
{
    while (*c->s == ' ' || *c->s == '\t')
        c->s++;
}

static double calc_parse_expr(calc_ctx_t *c);   // forward (parens recurse)

static double calc_parse_primary(calc_ctx_t *c)
{
    calc_skipws(c);
    char ch = *c->s;

    // unary +/-
    if (ch == '+' || ch == '-')
    {
        c->s++;
        if (++c->depth > CALC_MAX_DEPTH) { c->ok = false; c->depth--; return 0.0; }
        double v = calc_parse_primary(c);
        c->depth--;
        return (ch == '-') ? -v : v;
    }

    // parenthesized sub-expression
    if (ch == '(')
    {
        c->s++;
        if (++c->depth > CALC_MAX_DEPTH) { c->ok = false; c->depth--; return 0.0; }
        double v = calc_parse_expr(c);
        calc_skipws(c);
        if (*c->s == ')') c->s++;
        else c->ok = false;
        c->depth--;
        return v;
    }

    // numeric literal
    if ((ch >= '0' && ch <= '9') || ch == '.')
    {
        char *end = NULL;
        double v = strtod(c->s, &end);
        if (end == c->s) { c->ok = false; return 0.0; }
        c->s = end;
        return v;
    }

    // identifier -> channel name
    if (isalpha((unsigned char)ch) || ch == '_')
    {
        const char *start = c->s;
        while (isalnum((unsigned char)*c->s) || *c->s == '_')
            c->s++;
        size_t len = (size_t)(c->s - start);
        double v = 0.0;
        if (!calc_resolve_name(c->cfg, start, len, &v)) { c->ok = false; return 0.0; }
        return v;
    }

    c->ok = false;
    return 0.0;
}

static double calc_parse_term(calc_ctx_t *c)
{
    double v = calc_parse_primary(c);
    while (c->ok)
    {
        calc_skipws(c);
        char op = *c->s;
        if (op != '*' && op != '/')
            break;
        c->s++;
        double rhs = calc_parse_primary(c);
        if (!c->ok)
            break;
        if (op == '*')
            v *= rhs;
        else
        {
            if (rhs == 0.0) { c->ok = false; break; }
            v /= rhs;
        }
    }
    return v;
}

static double calc_parse_expr(calc_ctx_t *c)
{
    double v = calc_parse_term(c);
    while (c->ok)
    {
        calc_skipws(c);
        char op = *c->s;
        if (op != '+' && op != '-')
            break;
        c->s++;
        double rhs = calc_parse_term(c);
        if (!c->ok)
            break;
        if (op == '+') v += rhs;
        else v -= rhs;
    }
    return v;
}

// Evaluate a calculated-channel expression over current source values. true + *result on success.
static bool calc_evaluate(autopid_config_t *cfg, const char *expr, double *result)
{
    if (!cfg || !expr || !result)
        return false;
    calc_ctx_t c = { .s = expr, .cfg = cfg, .depth = 0, .ok = true };
    double v = calc_parse_expr(&c);
    calc_skipws(&c);
    if (!c.ok || *c.s != '\0')      // parse error or trailing garbage
        return false;
    if (!isfinite(v))
        return false;
    *result = v;
    return true;
}

void autopid_eval_calculated_channels(void)
{
    if (!autopid_config || autopid_config->calculated_count == 0)
        return;
    if (!autopid_lock(20))
        return;

    for (uint32_t i = 0; i < autopid_config->calculated_count; i++)
    {
        parameter_t *c = &autopid_config->calculated[i];
        if (!c->enabled || !c->name || c->name[0] == '\0' ||
            !c->expression || c->expression[0] == '\0')
            continue;
        double result = 0.0;
        if (!calc_evaluate(autopid_config, c->expression, &result))
            continue;                       // unknown/stale input or parse error -> skip this sweep
        if (c->min != FLT_MAX && result < (double)c->min)
            continue;
        if (c->max != FLT_MAX && result > (double)c->max)
            continue;
        c->value = (float)result;
        csv_logger_record(c->name, c->value, c->unit, "CALC");
    }

    autopid_unlock();
}

const std_pid_t *get_pid_from_string(const char *pid_string)
{
    char pid_hex[3];
    uint8_t pid_value;

    // Extract first 2 characters (hex PID)
    strncpy(pid_hex, pid_string, 2);
    pid_hex[2] = '\0';

    // Convert hex string to integer
    pid_value = (uint8_t)strtol(pid_hex, NULL, 16);

    // Get the base PID info
    const std_pid_t *pid_info = get_pid(pid_value);
    if (!pid_info)
    {
        return NULL;
    }

    // If there's a parameter name specified after the dash
    if (strchr(pid_string, '-'))
    {
        const char *param_name = strchr(pid_string, '-') + 1;

        // For all PIDs, verify the parameter exists
        bool param_found = false;
        for (int i = 0; i < pid_info->num_params; i++)
        {
            if (strcmp(pid_info->params[i].name, param_name) == 0)
            {
                param_found = true;
                break;
            }
        }
        if (!param_found)
        {
            return NULL;
        }
    }

    return pid_info;
}

char *autopid_data_read(void)
{
    // json_str must be freed by the caller
    static char *json_str = NULL;

    if (!autopid_data.mutex)
    {
        ESP_LOGE(TAG, "Invalid mutex, autopid_data not initialized");
        return NULL;
    }

    if (xSemaphoreTake(autopid_data.mutex, portMAX_DELAY) == pdTRUE)
    {
        if (autopid_data.json_str != NULL)
        {
            json_str = (char *)heap_caps_malloc(strlen(autopid_data.json_str) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (json_str != NULL)
            {
                strcpy(json_str, autopid_data.json_str);
            }
        }
        else
        {
            json_str = NULL;
        }
        xSemaphoreGive(autopid_data.mutex);
    }
    return json_str;
}

char *autopid_get_value_by_name(char *name)
{
    // json_str must be freed by the caller
    char *result_json = NULL;

    if (autopid_data.mutex == NULL)
    {
        ESP_LOGE(TAG, "Invalid mutex, autopid_data not initialized");
        return NULL;
    }
    else if (name == NULL || strlen(name) == 0)
    {
        ESP_LOGE(TAG, "Invalid name");
        return NULL;
    }

    if (xSemaphoreTake(autopid_data.mutex, portMAX_DELAY) == pdTRUE)
    {
        if (autopid_data.json_str != NULL)
        {
            char *json_copy = (char *)heap_caps_malloc(strlen(autopid_data.json_str) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (json_copy != NULL)
            {
                strcpy(json_copy, autopid_data.json_str);

                cJSON *root = cJSON_Parse(json_copy);
                if (root)
                {
                    cJSON *item = cJSON_GetObjectItem(root, name);
                    if (item)
                    {
                        cJSON *result = cJSON_CreateObject();
                        cJSON *value_copy = cJSON_Duplicate(item, 1);
                        cJSON_AddItemToObject(result, name, value_copy);
                        result_json = cJSON_PrintUnformatted(result);
                        cJSON_Delete(result);
                    }
                    cJSON_Delete(root);
                }
                heap_caps_free(json_copy);
            }
        }
        xSemaphoreGive(autopid_data.mutex);
    }
    return result_json;
}

bool autopid_get_ecu_status(void)
{
    EventBits_t uxBits;
    if (xautopid_event_group != NULL)
    {
        uxBits = xEventGroupGetBits(xautopid_event_group);

        return (uxBits & ECU_CONNECTED_BIT);
    }
    else
        return false;
}

// FAST_LOG (Task #18): load the channel config (load + mutex + provider) and nothing else.
// Lets components/fast_log read can_filters[]/pids[] and reuse autopid_lock()/
// autopid_collect_log_columns while the native-TWAI capture owns the bus.
autopid_config_t *autopid_load_config_only(void)
{
    // Idempotent: if this was called before, reuse it.
    if (autopid_config != NULL)
    {
        return autopid_config;
    }

    // Register the wide-CSV column provider so the CSV writer can enumerate channels
    // through the function pointer with no build-time dep.
    csv_logger_set_column_provider(autopid_collect_log_columns);

    autopid_config = load_autopid_config();
    if (autopid_config == NULL)
    {
        ESP_LOGE(TAG, "autopid_load_config_only: load_autopid_config returned NULL");
        return NULL;
    }

    s_autopid_mutex = xSemaphoreCreateMutex();
    if (s_autopid_mutex == NULL)
    {
        ESP_LOGE(TAG, "autopid_load_config_only: failed to create config mutex");
        return NULL;
    }
    if (s_autopid_file_lock == NULL)
    {
        s_autopid_file_lock = xSemaphoreCreateMutex();
    }

    if (autopid_data.mutex == NULL)
    {
        autopid_data.mutex = xSemaphoreCreateMutex();
    }

    ESP_LOGI(TAG, "autopid_load_config_only: %lu pids, %lu can_filters",
             (unsigned long)autopid_config->pid_count,
             (unsigned long)autopid_config->can_filters_count);
    return autopid_config;
}
