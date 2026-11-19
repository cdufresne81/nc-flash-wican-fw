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

#include "autopid_http_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_log.h>
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "autopid.h"
#include "config_server.h"

static const char *TAG = "AUTOPID_HTTP";

static StaticSemaphore_t test_canflt_lock_buf;
static SemaphoreHandle_t test_canflt_lock = NULL;

static char *read_json_body(httpd_req_t *req, size_t *out_len)
{
    if (!req || !out_len)
        return NULL;
    *out_len = 0;

    size_t len = req->content_len;
    if (len == 0)
        return NULL;

    // Hard cap to avoid huge allocations
    if (len > 1024)
        len = 1024;

    char *buf = NULL;
#ifdef CONFIG_SPIRAM
    buf = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    buf = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_8BIT);
#endif
    if (!buf)
        return NULL;

    size_t off = 0;
    while (off < len)
    {
        int r = httpd_req_recv(req, buf + off, (int)(len - off));
        if (r <= 0)
        {
            heap_caps_free(buf);
            return NULL;
        }
        off += (size_t)r;
    }

    buf[off] = '\0';
    *out_len = off;
    return buf;
}

static bool parse_frame_id_any(const cJSON *j, uint32_t *out)
{
    if (!out)
        return false;
    *out = 0;

    if (!j)
        return false;

    if (cJSON_IsNumber(j))
    {
        if (j->valuedouble < 0 || j->valuedouble > 0x1FFFFFFF)
            return false;
        *out = (uint32_t)j->valuedouble;
        return true;
    }

    if (!cJSON_IsString(j) || !j->valuestring)
        return false;

    const char *s = j->valuestring;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return false;

    int base = 10;
    if ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X'))
    {
        base = 16;
        s += 2;
    }
    else
    {
        // If it contains hex letters, treat as hex
        for (const char *p = s; *p; p++)
        {
            if ((*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f'))
            {
                base = 16;
                break;
            }
        }
    }

    char *endptr = NULL;
    unsigned long v = strtoul(s, &endptr, base);
    if (endptr == s)
        return false;

    if (v > 0x1FFFFFFF)
        return false;

    *out = (uint32_t)v;
    return true;
}

/* POLL_LOG live-test path (issue #41). Passive broadcast capture. No autopid_lock / no ELM327:
 * parse frame_id+expr, hand a one-shot to the poll task via autopid_live_test(), map to the
 * {ok,value,error,raw} JSON runCanFilterTest consumes. Works even while the bus is quiesced
 * (capture never transmits). */
static esp_err_t test_can_filter_handler_polllog(httpd_req_t *req)
{
    if (test_canflt_lock == NULL)
        test_canflt_lock = xSemaphoreCreateMutexStatic(&test_canflt_lock_buf);
    if (xSemaphoreTake(test_canflt_lock, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Busy\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    size_t body_len = 0;
    char *body = read_json_body(req, &body_len);
    if (!body)
    {
        xSemaphoreGive(test_canflt_lock);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Missing JSON\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    cJSON *json = cJSON_Parse(body);
    heap_caps_free(body);
    if (!json)
    {
        xSemaphoreGive(test_canflt_lock);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Invalid JSON\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const cJSON *jfid  = cJSON_GetObjectItemCaseSensitive(json, "frame_id");
    const cJSON *jexpr = cJSON_GetObjectItemCaseSensitive(json, "expr");
    uint32_t frame_id = 0;
    char expr[128] = {0};
    bool bad = false;
    if (!parse_frame_id_any(jfid, &frame_id) || frame_id == 0)
        bad = true;
    if (!cJSON_IsString(jexpr) || !jexpr->valuestring || jexpr->valuestring[0] == '\0')
        bad = true;
    else
        strlcpy(expr, jexpr->valuestring, sizeof(expr));
    cJSON_Delete(json);

    if (bad)
    {
        xSemaphoreGive(test_canflt_lock);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Invalid frame_id/expr\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    autopid_live_test_req_t treq = {
        .kind = AUTOPID_LIVE_TEST_CANFLT,
        .frame_id = frame_id,
        .is_extended = (frame_id > 0x7FFu),   // inferred, same rule as can_filter_t
    };
    strlcpy(treq.expr, expr, sizeof(treq.expr));
    autopid_live_test_res_t tres = {0};   // defense in depth: never switch on an unset status
    autopid_live_test(&treq, &tres, 3000);

    cJSON *root = cJSON_CreateObject();
    switch (tres.status)
    {
    case AUTOPID_TEST_DONE:
        cJSON_AddBoolToObject(root, "ok", tres.ok);
        if (tres.ok)
            cJSON_AddNumberToObject(root, "value", tres.value);
        else
            cJSON_AddStringToObject(root, "error", tres.error[0] ? tres.error : "Failed");
        if (tres.raw[0])
            cJSON_AddStringToObject(root, "raw", tres.raw);
        break;
    case AUTOPID_TEST_TRIP_BUSY:
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "code", "trip_running");
        cJSON_AddStringToObject(root, "error",
            "A trip log is running - tests can't run during a trip. Stop the trip and retry, or abort.");
        break;
    case AUTOPID_TEST_TIMEOUT:
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "Adapter busy - try again.");
        break;
    case AUTOPID_TEST_INACTIVE:
    default:
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "Set Protocol to AutoPID or Poll Log and Submit Changes");
        break;
    }
    // ENGINE_OFF cannot occur for CANFLT: passive capture works while quiesced.

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (out)
    {
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        cJSON_free(out);
    }
    else
    {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"OOM\"}", HTTPD_RESP_USE_STRLEN);
    }

    xSemaphoreGive(test_canflt_lock);
    return ESP_OK;
}

static esp_err_t test_can_filter_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP /autopid/test_can_filter hit");

    int proto = config_server_protocol();
    if (proto == POLL_LOG)                 // issue #41: passive capture in-band on the poll_log task
        return test_can_filter_handler_polllog(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"Set Protocol to AutoPID or Poll Log and Submit Changes\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t autopid_http_register_test_can_filter(httpd_handle_t server)
{
    static const httpd_uri_t test_can_uri_post = {
        .uri = "/autopid/test_can_filter",
        .method = HTTP_POST,
        .handler = test_can_filter_handler,
        .user_ctx = NULL,
    };

    if (test_canflt_lock == NULL)
    {
        test_canflt_lock = xSemaphoreCreateMutexStatic(&test_canflt_lock_buf);
    }

    esp_err_t r = httpd_register_uri_handler(server, &test_can_uri_post);
    if (r != ESP_OK && r != ESP_ERR_HTTPD_HANDLER_EXISTS)
        return r;

    ESP_LOGI(TAG, "AutoPID handlers registered at /autopid/test_can_filter");
    return ESP_OK;
}
