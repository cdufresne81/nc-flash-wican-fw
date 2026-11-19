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

static StaticSemaphore_t test_pid_lock_buf;
static SemaphoreHandle_t test_pid_lock = NULL;

static char *read_json_body(httpd_req_t *req, size_t *out_len)
{
    if (!req || !out_len)
        return NULL;
    *out_len = 0;

    size_t len = req->content_len;
    if (len == 0)
        return NULL;

    // Hard cap to avoid huge allocations
    if (len > 1024*10)
        len = 1024*10;

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

/* POLL_LOG live-test path (issue #41). Under the poll_log protocol the ELM327 stack is dormant
 * and the poll task owns the TWAI bus, so we do NOT take autopid_lock or touch ELM327 here:
 * parse pid+expr, hand a one-shot to the poll task via autopid_live_test(), and map the result
 * to the {ok,value,error,raw} JSON the runPidTest UI consumes.
 * `init`/`pid_init`/`rxheader` are intentionally ignored -- the native poller uses fixed
 * physical addressing (0x7E0->0x7E8) with no ELM AT setup, so the test reflects exactly what
 * poll_log will read. */
static esp_err_t test_pid_handler_polllog(httpd_req_t *req)
{
    if (test_pid_lock == NULL)
        test_pid_lock = xSemaphoreCreateMutexStatic(&test_pid_lock_buf);
    if (xSemaphoreTake(test_pid_lock, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Busy\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char pid_cmd[64] = {0};
    char expr[128]   = {0};

    // POST JSON body takes precedence; fall back to GET query params.
    size_t body_len = 0;
    char *body = read_json_body(req, &body_len);
    if (body)
    {
        cJSON *j = cJSON_Parse(body);
        if (j)
        {
            const cJSON *jpid = cJSON_GetObjectItemCaseSensitive(j, "pid");
            const cJSON *je   = cJSON_GetObjectItemCaseSensitive(j, "expr");
            if (cJSON_IsString(jpid) && jpid->valuestring)
                snprintf(pid_cmd, sizeof(pid_cmd), "%s", jpid->valuestring);
            if (cJSON_IsString(je) && je->valuestring)
                snprintf(expr, sizeof(expr), "%s", je->valuestring);
            cJSON_Delete(j);
        }
        heap_caps_free(body);
    }
    else
    {
        size_t qlen = httpd_req_get_url_query_len(req);
        if (qlen > 0)
        {
            char *query = (char *)heap_caps_malloc(qlen + 1, MALLOC_CAP_8BIT);
            if (query && httpd_req_get_url_query_str(req, query, qlen + 1) == ESP_OK)
            {
                (void)httpd_query_key_value(query, "pid", pid_cmd, sizeof(pid_cmd));
                (void)httpd_query_key_value(query, "expr", expr, sizeof(expr));
            }
            if (query)
                heap_caps_free(query);
        }
    }

    if (pid_cmd[0] == '\0' || expr[0] == '\0')
    {
        xSemaphoreGive(test_pid_lock);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Missing pid/expr\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    autopid_live_test_req_t treq = { .kind = AUTOPID_LIVE_TEST_PID };
    strlcpy(treq.cmd, pid_cmd, sizeof(treq.cmd));
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
    case AUTOPID_TEST_ENGINE_OFF:
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "code", "engine_off");
        cJSON_AddStringToObject(root, "error", "Engine off - start the engine to test a polled PID.");
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

    xSemaphoreGive(test_pid_lock);
    return ESP_OK;
}

static esp_err_t test_pid_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP /autopid/test_pid hit (qlen=%u)", (unsigned)httpd_req_get_url_query_len(req));

    int proto = config_server_protocol();
    if (proto == POLL_LOG)                 // issue #41: run the test in-band on the poll_log task
        return test_pid_handler_polllog(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"Set Protocol to AutoPID or Poll Log and Submit Changes\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t autopid_http_register_test_pid(httpd_handle_t server)
{
    static const httpd_uri_t test_pid_uri = {
        .uri = "/autopid/test_pid",
        .method = HTTP_GET,
        .handler = test_pid_handler,
        .user_ctx = NULL,
    };

    static const httpd_uri_t test_pid_uri_post = {
        .uri = "/autopid/test_pid",
        .method = HTTP_POST,
        .handler = test_pid_handler,
        .user_ctx = NULL,
    };

    if (test_pid_lock == NULL)
    {
        test_pid_lock = xSemaphoreCreateMutexStatic(&test_pid_lock_buf);
    }

    esp_err_t r = httpd_register_uri_handler(server, &test_pid_uri);
    if (r != ESP_OK && r != ESP_ERR_HTTPD_HANDLER_EXISTS)
        return r;

    r = httpd_register_uri_handler(server, &test_pid_uri_post);
    if (r != ESP_OK && r != ESP_ERR_HTTPD_HANDLER_EXISTS)
        return r;

    ESP_LOGI(TAG, "AutoPID handlers registered at /autopid/test_pid");
    return ESP_OK;
}
