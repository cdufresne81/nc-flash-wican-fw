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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* NC Flash logging session -- the coordinator. See datalog_session.h and
 * docs/internals/datalog_session.md. The rules it applies are in datalog_session_logic.c
 * (host-tested); this file owns the lock, the HTTP handlers and the side effects. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "autopid.h"
#include "can.h"
#include "config_server.h"
#include "csv_logger.h"
#include "csv_bringup_logic.h"
#include "datalog_session.h"
#include "datalog_session_logic.h"
#include "event_log.h"
#include "poll_log.h"

#define TAG "datalog_session"

/* One lock for the whole session: the state below AND the side effects that go with a
 * transition (the RAM list, the CSV manual mode, the reload request). A begin, an end and the
 * lease reaper therefore never interleave, so the list in RAM always belongs to the token in
 * s_st. Takers: the httpd task (every /datalog_session request), datalog_lease_task (expiry,
 * 1 Hz) and the sleep task (teardown). Never the poll task or the CSV writer -- holding it can
 * wait on the autopid file lock for the length of a table parse.
 * Created once in datalog_session_register_handlers(), before any taker can exist. */
static SemaphoreHandle_t s_lock = NULL;
static StaticSemaphore_t s_lock_buf;
static dls_state_t s_st;   /* guarded by s_lock */

/* Lock-free copy of s_st.active, so the 1 Hz tick and the sleep teardown cost one read when
 * there is nothing to do. Written only under s_lock; a plain aligned bool. A reader that sees
 * a stale value takes the lock and looks at s_st, or skips one tick. */
static volatile bool s_active = false;

static inline uint32_t dls_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool dls_lock(void)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void dls_unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* End the active session. Caller holds s_lock and has checked s_st.active.
 * Order matters: the list goes first, so the reload queued last can only find the saved file. */
static void dls_end_locked(dls_end_t reason)
{
    const int8_t saved_mode = dls_end(&s_st, reason);
    s_active = false;

    autopid_session_list_set(NULL, 0);
    /* Obeys the park rule: under a host park the mode is left alone and the value goes into
     * the park's pre-pause slot, so the producer cannot restart under a parked host. */
    csv_logger_restore_manual_mode(saved_mode);
    /* Back to auto_pid.json. Under a park or a sleep the poller is not running its safe
     * point; the request waits and is served before its first sweep afterwards. */
    poll_log_request_reload_roll();

    event_log_emit(EVL_INFO, "NC Flash channel list ended (%s) -- logging the saved list again",
                   (reason == DLS_END_HOST)    ? "logging stopped" :
                   (reason == DLS_END_EXPIRED) ? "NC Flash stopped answering" :
                                                 "going to sleep");
}

void datalog_session_tick(void)
{
    if (!s_active || !dls_lock()) { return; }
    if (dls_expired(&s_st, dls_now_ms()))
    {
        dls_end_locked(DLS_END_EXPIRED);
    }
    dls_unlock();
}

void datalog_session_end_for_sleep(void)
{
    if (!s_active || !dls_lock()) { return; }
    if (s_st.active)
    {
        dls_end_locked(DLS_END_SLEEP);
    }
    dls_unlock();
}

/* ---- HTTP ----------------------------------------------------------------------------- */

static esp_err_t dls_send(httpd_req_t *req, const char *status, const char *body)
{
    if (status != NULL) { httpd_resp_set_status(req, status); }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* error and msg are fixed literals (never host input), so no JSON escaping is needed. */
static esp_err_t dls_send_error(httpd_req_t *req, const char *status, const char *error,
                                const char *msg)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\",\"msg\":\"%s\"}", error, msg);
    return dls_send(req, status, body);
}

static esp_err_t dls_send_stale(httpd_req_t *req)
{
    return dls_send(req, "409 Conflict", "{\"ok\":false,\"error\":\"stale\"}");
}

static bool dls_query_u32(const char *query, const char *key, uint32_t *out)
{
    char buf[16];
    if (httpd_query_key_value(query, key, buf, sizeof(buf)) != ESP_OK) { return false; }
    *out = (uint32_t)strtoul(buf, NULL, 10);
    return true;
}

/* Read the whole body into a PSRAM buffer (NUL-terminated). NULL on any failure, with *why set
 * to a fixed sentence for the reply; *why stays NULL when the failure is ours (out of memory). */
static char *dls_read_body(httpd_req_t *req, int *len_out, const char **why)
{
    const int total = (int)req->content_len;
    if (total <= 0 || total > DLS_BODY_MAX_BYTES)
    {
        *why = "The channel list is missing or larger than 64 KB.";
        return NULL;
    }
    char *buf = heap_caps_malloc((size_t)total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL)
    {
        return NULL;
    }

    int got = 0;
    int timeouts = 0;
    while (got < total)
    {
        const int r = httpd_req_recv(req, buf + got, (size_t)(total - got));
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3)
        {
            continue;   /* a slow link; a host that stalls for good is given up on */
        }
        if (r <= 0)
        {
            free(buf);
            *why = "The channel list did not arrive completely.";
            return NULL;
        }
        got += r;
    }
    buf[total] = '\0';
    *len_out = total;
    return buf;
}

/* Is this a channel list we are willing to hand to the table parser? Returns the number of
 * rows in "pids", or -1. The parser itself tolerates missing can_filters / calculated and odd
 * values; what it is not asked to survive is deep nesting (it recurses, and runs again on the
 * poll task) or a document with no "pids" array at all, which would silently log nothing. */
static int dls_validate_list(const char *json, int len)
{
    if (!dls_json_depth_ok(json, (size_t)len, DLS_JSON_MAX_DEPTH)) { return -1; }

    cJSON *root = cJSON_ParseWithLength(json, (size_t)len);
    if (root == NULL) { return -1; }
    const cJSON *pids = cJSON_IsObject(root) ? cJSON_GetObjectItem(root, "pids") : NULL;
    const int rows = cJSON_IsArray(pids) ? cJSON_GetArraySize(pids) : -1;
    cJSON_Delete(root);
    return rows;
}

static esp_err_t dls_handle_begin(httpd_req_t *req, const char *query)
{
    const char *why = NULL;
    int len = 0;
    char *body = dls_read_body(req, &len, &why);
    if (body == NULL)
    {
        return (why != NULL)
                   ? dls_send_error(req, "400 Bad Request", "bad_request", why)
                   : dls_send_error(req, "500 Internal Server Error", "internal", "Out of memory.");
    }
    const int rows = dls_validate_list(body, len);
    if (rows < 0)
    {
        free(body);
        return dls_send_error(req, "400 Bad Request", "bad_request",
                              "The channel list is not valid JSON with a pids array.");
    }

    uint32_t ttl_s = 0;
    const bool ttl_given = dls_query_u32(query, "ttl_s", &ttl_s);
    ttl_s = dls_clamp_ttl_s(ttl_given, ttl_s);

    if (!dls_lock())
    {
        free(body);
        return dls_send_error(req, "500 Internal Server Error", "internal", "Not ready.");
    }

    /* Refuse while anything else owns the bus. Checked under the lock and as late as possible:
     * from here to the reply nothing waits on the network. A park that begins after this point
     * is handled by the session's own rules (the reload waits, an end restores through the
     * park's pre-pause slot); this check only keeps a session from STARTING under one. */
    const dls_begin_in_t in = {
        .fence_clear    = (config_server_flash_fence() == FLASH_FENCE_CLEAR),
        .datalog_parked = can_datalog_park_active(),
        .bus_claimed    = can_host_bus_claim_active(),
        .sleeping       = can_sleep_fence_active(),
        .poll_active    = poll_log_active(),
    };
    const dls_begin_check_t check = dls_begin_check(&in);
    if (check != DLS_BEGIN_OK)
    {
        dls_unlock();
        free(body);
        return dls_send_error(req, "409 Conflict",
                              (check == DLS_BEGIN_BUSY) ? "busy" : "inactive",
                              dls_begin_refusal_msg(check));
    }

    const bool replacing = s_st.active;

    /* Hold the writer BEFORE forcing it on: otherwise the first record after the force opens a
     * trip on the OLD columns, and the roll then closes it again -- a stray near-empty file on
     * every run started with the engine off. The poll task lowers the hold after the swap. */
    const int8_t mode_before = csv_logger_manual_mode();
    csv_logger_reload_hold(true);
    if (csv_logger_set_manual_override(true) != ESP_OK)
    {
        /* The writer task could not be created (out of memory). That can only happen before
         * any session has run, so nothing else needs undoing. */
        csv_logger_reload_hold(false);
        csv_logger_restore_manual_mode(mode_before);
        dls_unlock();
        free(body);
        return dls_send_error(req, "500 Internal Server Error", "internal",
                              "The trip logger could not be started.");
    }

    const esp_err_t set_err = autopid_session_list_set(body, (size_t)len);
    free(body);
    if (set_err != ESP_OK)
    {
        /* Out of memory for the copy. A session that was already running keeps its list, its
         * token and its forced-on logger; a first begin gets its mode back. */
        csv_logger_reload_hold(false);
        if (!replacing) { csv_logger_restore_manual_mode(mode_before); }
        dls_unlock();
        return dls_send_error(req, "500 Internal Server Error", "internal", "Out of memory.");
    }

    const uint32_t token = dls_begin(&s_st, dls_now_ms(), ttl_s, mode_before, esp_random());
    s_active = true;
    /* Cannot fail: poll_log_active() was true above and the poller never stops once started. */
    poll_log_request_reload_roll();
    dls_unlock();

    event_log_emit(EVL_INFO, replacing
                       ? "NC Flash changed its channel list (%d channels)"
                       : "NC Flash is logging its own channel list (%d channels) -- the saved "
                         "list is set aside until it finishes",
                   rows);

    char out[96];
    snprintf(out, sizeof(out), "{\"ok\":true,\"active\":true,\"token\":%lu,\"ttl_s\":%lu}",
             (unsigned long)token, (unsigned long)ttl_s);
    return dls_send(req, NULL, out);
}

/* POST /datalog_session?op=begin|keepalive|end -- see docs/internals/datalog_session.md. */
static esp_err_t datalog_session_post_handler(httpd_req_t *req)
{
    char query[96];
    char op[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "op", op, sizeof(op)) != ESP_OK)
    {
        return dls_send_error(req, "400 Bad Request", "bad_request", "Missing op.");
    }

    if (strcmp(op, "begin") == 0)
    {
        return dls_handle_begin(req, query);
    }

    const bool keepalive = (strcmp(op, "keepalive") == 0);
    if (!keepalive && strcmp(op, "end") != 0)
    {
        return dls_send_error(req, "400 Bad Request", "bad_request",
                              "op must be begin, keepalive or end.");
    }

    /* A missing token is token 0, which never matches: stale, like any other wrong token. */
    uint32_t token = 0;
    (void)dls_query_u32(query, "token", &token);

    if (!dls_lock())
    {
        return dls_send_stale(req);
    }
    char out[80];
    bool ok;
    if (keepalive)
    {
        const uint32_t now = dls_now_ms();
        ok = dls_keepalive(&s_st, token, now);
        snprintf(out, sizeof(out), "{\"ok\":true,\"active\":true,\"remaining_ms\":%lu}",
                 (unsigned long)dls_remaining_ms(&s_st, now));
    }
    else
    {
        ok = dls_token_matches(&s_st, token);
        if (ok) { dls_end_locked(DLS_END_HOST); }
        snprintf(out, sizeof(out), "{\"ok\":true,\"active\":false}");
    }
    dls_unlock();

    return ok ? dls_send(req, NULL, out) : dls_send_stale(req);
}

/* GET /datalog_session -- is a session active, how long its lease has left, why the last ended. */
static esp_err_t datalog_session_get_handler(httpd_req_t *req)
{
    bool active = false;
    uint32_t remaining_ms = 0;
    dls_end_t last_end = DLS_END_NONE;
    if (dls_lock())
    {
        active = s_st.active;
        remaining_ms = dls_remaining_ms(&s_st, dls_now_ms());
        last_end = s_st.last_end;
        dls_unlock();
    }

    char out[112];
    snprintf(out, sizeof(out), "{\"ok\":true,\"active\":%s,\"remaining_ms\":%lu,\"last_end\":\"%s\"}",
             active ? "true" : "false", (unsigned long)remaining_ms, dls_end_name(last_end));
    return dls_send(req, NULL, out);
}

static const httpd_uri_t datalog_session_post_uri = {
    .uri = "/datalog_session",
    .method = HTTP_POST,
    .handler = datalog_session_post_handler,
    .user_ctx = NULL
};

static const httpd_uri_t datalog_session_get_uri = {
    .uri = "/datalog_session",
    .method = HTTP_GET,
    .handler = datalog_session_get_handler,
    .user_ctx = NULL
};

void datalog_session_register_handlers(httpd_handle_t server)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    httpd_register_uri_handler(server, &datalog_session_post_uri);
    httpd_register_uri_handler(server, &datalog_session_get_uri);
}
