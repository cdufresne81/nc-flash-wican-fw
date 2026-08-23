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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

#include "event_log.h"

static const char *TAG = "event_log";

// SD layout. EVENT_LOG_DIR ("/sdcard/events") is defined in event_log.h (shared with sd_filemgr, which
// marks it a protected dir). A fopen under it returns NULL (handled) when no card is mounted; event_log
// is a leaf and never reads main's SD_CARD_MOUNT_POINT macro.
#define EVENT_LOG_FILE         EVENT_LOG_DIR "/events.log"

// Tunables. Events are sparse (boot, a few engine/datalog transitions per drive, rare OTA), so the
// ring is small and the writer is a low-priority, infrequently-woken task.
#define EVENT_LOG_RING_N       64           // in-RAM ring depth (last N events, always available)
#define EVENT_LOG_LINE_MAX     192          // one fully-formatted line (no trailing newline)
#define EVENT_LOG_DETAIL_MAX   112          // printf detail portion
#define EVENT_LOG_ROTATE_BYTES (128 * 1024) // rotate the active file past this size
#define EVENT_LOG_KEEP_FILES   4            // events.1.log .. events.<KEEP>.log kept after rotation
#define EVENT_LOG_WRITER_PRIO  2            // below csv writer (4) and poll_log (5): never steals hot cycles
#define EVENT_LOG_WRITER_STACK (4096)
#define EVENT_LOG_WAKE_MS      1000         // writer wakes at least this often (also a flush tick)
#define EVENT_LOG_GUARD_STABLE_US (15 * 1000 * 1000)

// Distinct RTC_NOINIT crash-guard magic (NOT csv 0xA11C0DE5 / poll_log 0x9011106D / fast_log 0xFA571A6D).
#define EVENT_LOG_GUARD_MAGIC  0xE7106A11u
RTC_NOINIT_ATTR static uint32_t s_evl_guard;

// True for the rest of this uptime when the guard above made us skip SD persistence. Without a
// reboot the event log would stay RAM-only for the whole uptime, silently -- which is exactly the
// evidence trail we rely on to debug the field. See sleep_mode_recovery_needed().
static bool s_bringup_skipped = false;

typedef struct {
    uint32_t seq;                       // 1-based emit sequence (0 = never written)
    char     line[EVENT_LOG_LINE_MAX];  // preformatted, NUL-terminated, no '\n'
} evl_entry_t;

// In-RAM ring in INTERNAL RAM (.bss). The writer reads it inside fwrite/fsync windows, so it must
// never live in PSRAM. A static array also cannot fail to allocate -- the most brick-safe choice.
static evl_entry_t s_ring[EVENT_LOG_RING_N];
static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_head_seq = 0;     // next slot to write (monotonic)
static volatile uint32_t s_persist_seq = 0;  // next slot to flush to SD (monotonic)
static volatile uint32_t s_dropped = 0;      // events overwritten before they reached SD
static volatile uint32_t s_bad_ctx = 0;      // emits from a context that must not format (#111)
static volatile uint32_t s_rotations = 0;
static volatile uint32_t s_file_bytes = 0;
static volatile bool     s_sd_ok = false;    // last SD write outcome (for status)

static SemaphoreHandle_t s_wake = NULL;      // emit() -> writer nudge
static SemaphoreHandle_t s_file_mtx = NULL;  // serializes SD file access (writer task + HTTP reader)
static bool s_inited = false;

static event_log_sd_ready_fn_t s_sd_ready_fn = NULL;

void event_log_set_sd_ready_fn(event_log_sd_ready_fn_t fn)
{
    s_sd_ready_fn = fn;   // plain store; written once at boot before the writer matters
}

static const char *evl_code_str(event_log_code_t code)
{
    // No default: every enumerator is listed so -Wswitch (under -Werror) turns a future
    // "added an event code, forgot its string" into a build error instead of a silent "EVENT"
    // mislabel in the post-hoc brick log. EVL_CODE_MAX is the count sentinel, not a real event.
    switch (code)
    {
        case EVL_BOOT:          return "BOOT";
        case EVL_MODE:          return "MODE";
        case EVL_IGNITION_ON:   return "IGNITION_ON";
        case EVL_IGNITION_OFF:  return "IGNITION_OFF";
        case EVL_ENGINE_ON:     return "ENGINE_ON";
        case EVL_ENGINE_OFF:    return "ENGINE_OFF";
        case EVL_DATALOG_OPEN:  return "DATALOG_OPEN";
        case EVL_DATALOG_CLOSE: return "DATALOG_CLOSE";
        case EVL_UPDATE_START:  return "UPDATE_START";
        case EVL_UPDATE_DONE:   return "UPDATE_DONE";
        case EVL_UPDATE_FAIL:   return "UPDATE_FAIL";
        case EVL_FLASH_START:   return "FLASH_START";
        case EVL_FLASH_OK:      return "FLASH_OK";
        case EVL_FLASH_FAIL:    return "FLASH_FAIL";
        case EVL_READ_START:    return "READ_START";
        case EVL_READ_OK:       return "READ_OK";
        case EVL_HOST_CLAIM:    return "HOST_CLAIM";
        case EVL_HOST_RELEASE:  return "HOST_RELEASE";
        case EVL_DATALOG_PARK:  return "DATALOG_PARK";
        case EVL_DATALOG_RESUME:return "DATALOG_RESUME";
        case EVL_REAPER_RESUME: return "REAPER_RESUME";
        case EVL_CAN_WAKE:      return "CAN_WAKE";
        case EVL_WIFI:          return "WIFI";
        case EVL_WARN:          return "WARN";
        case EVL_INFO:          return "INFO";
        case EVL_CODE_MAX:      break;
    }
    return "EVENT";
}

// Debug-detail gate (#98). Mirrors the stored "debug" config flag, pushed in by main (the
// event_log component must not depend on config_server -- main depends on components, not the
// reverse). volatile + single 32-bit-atomic writer: main at boot, cmd_debug at runtime.
static volatile bool s_debug_events = false;

void event_log_set_debug(bool on)
{
    s_debug_events = on;
}

bool event_log_debug_enabled(void)
{
    return s_debug_events;
}

// ---- Caller-context tripwire (#111) ------------------------------------------------------------

/* Emitting formats the whole line on the CALLER's stack -- ~800 bytes once vsnprintf, localtime_r
 * and strftime are counted (see the rule in event_log.h). Three contexts cannot afford that: the
 * system event task and the esp_timer task, whose stacks are Kconfig-sized and small, and an ISR,
 * which has none to spare. Doing it on sys_evt boot-looped a device.
 *
 * That rule used to live only in a header comment, which catches nobody. This is the mechanism:
 * a couple of pointer compares per emit, on a path that runs at milestone rate. It names the
 * OFFENDER (the task, and the event code) rather than the victim, and it fires on the first
 * offending call instead of waiting for a stack floor to be crossed -- which is what the sibling
 * check in wifi_diag can no longer reliably do, now that #111 freed up the headroom it watched.
 *
 * Returns false when the caller must NOT proceed. That is the ISR case only, and it is not a
 * judgement call: the emit path below takes a portMUX in its task form and calls xSemaphoreGive,
 * neither of which is legal from an ISR, so continuing means an assert or corrupted state. A line
 * that cannot survive being written is not preserved by attempting to write it.
 *
 * A bad TASK proceeds, on purpose. It might be the emit that overflows -- but the line may also be
 * the only forensic record of whatever went wrong, and a diagnostic facility must never be the
 * thing that takes the device down. ESP_EARLY_LOGE, not ESP_LOGE: it goes through the ROM printf
 * and is far shallower, which matters when the whole complaint is that this stack is nearly full.
 *
 * Do not expect to READ either log line: this board has no serial console anyone can attach (its
 * USB-C port is a USB host at runtime), and the crash reporter captures the RTC panic backtrace,
 * not console output. The durable signal is "bad_ctx" in GET /event_log/status -- non-zero there
 * means someone broke the rule and the device lived to report it. If it did not live, the
 * backtrace is still the evidence, exactly as in v1.19.1. */
static bool evl_caller_may_format(event_log_code_t code)
{
    if (xPortInIsrContext())
    {
        s_bad_ctx++;
        ESP_DRAM_LOGE(TAG, "event_log_emit from an ISR (code %d) dropped -- capture and defer",
                      (int)code);
        return false;
    }

    /* Looked up once each and cached. xTaskGetHandle suspends the scheduler to walk the task
     * lists, so it must not run per emit forever -- and it is illegal from an ISR, which is why
     * that check comes first. A task that does not exist yet leaves its handle NULL and is simply
     * not compared against; that is normal early in boot, not an error. */
    static TaskHandle_t s_sys_evt = NULL;
    static TaskHandle_t s_esp_timer = NULL;
    if (s_sys_evt == NULL)   s_sys_evt = xTaskGetHandle("sys_evt");     /* IDF default event loop */
    if (s_esp_timer == NULL) s_esp_timer = xTaskGetHandle("esp_timer"); /* IDF timer dispatch     */

    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    const char *who = (self == s_sys_evt && s_sys_evt != NULL)       ? "sys_evt"
                    : (self == s_esp_timer && s_esp_timer != NULL)   ? "esp_timer"
                    : NULL;
    if (who != NULL)
    {
        s_bad_ctx++;
        ESP_EARLY_LOGE(TAG, "event_log_emit on %s (code %d) -- this is what boot-looped v1.19.1; "
                            "capture the facts there and format on a task that owns its stack "
                            "(components/wifi_diag/wifi_diag.c is the worked example)",
                       who, (int)code);
    }
    return true;
}

// Shared core for the public emit entries, so the ring critical section exists in one place.
//
// at_tv / at_up_ms carry the time the event HAPPENED, for deferred emitters (see
// event_log_emit_at). Pass at_tv == NULL for "now", which is what event_log_emit() does and what
// every direct caller has always had.
static void evl_vemit(event_log_code_t code, const struct timeval *at_tv, int64_t at_up_ms,
                      const char *fmt, va_list ap)
{
    if (!evl_caller_may_format(code))
    {
        return;   /* ISR only: proceeding would be illegal, not merely expensive */
    }

    char detail[EVENT_LOG_DETAIL_MAX];
    if (fmt != NULL)
    {
        vsnprintf(detail, sizeof(detail), fmt, ap);
    }
    else
    {
        detail[0] = '\0';
    }

    // Wall-clock (when synced) + monotonic uptime, both computed OUTSIDE the critical section.
    int64_t up_ms;
    char ts[24];
    struct timeval tv;
    struct tm tm_now;
    if (at_tv != NULL)
    {
        tv = *at_tv;                              // the event's time, captured by the emitter
        up_ms = at_up_ms;
    }
    else
    {
        up_ms = esp_timer_get_time() / 1000;
        gettimeofday(&tv, NULL);
    }
    localtime_r(&tv.tv_sec, &tm_now);
    if ((tm_now.tm_year + 1900) >= 2020)
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
    else
        strlcpy(ts, "unsynced", sizeof(ts));

    char line[EVENT_LOG_LINE_MAX];
    snprintf(line, sizeof(line), "%s up=%lldms %-12s %s",
             ts, (long long)up_ms, evl_code_str(code), detail);

    // Publish into the ring. Brief critical section: one strlcpy + a couple of counter updates.
    portENTER_CRITICAL(&s_ring_lock);
    uint32_t idx = s_head_seq % EVENT_LOG_RING_N;
    strlcpy(s_ring[idx].line, line, sizeof(s_ring[idx].line));
    s_head_seq++;
    s_ring[idx].seq = s_head_seq;   // 1-based
    portEXIT_CRITICAL(&s_ring_lock);

    if (s_wake != NULL)
    {
        xSemaphoreGive(s_wake);   // non-blocking nudge; safe from any task
    }

    ESP_LOGI(TAG, "%s", line);    // also visible on the console
}

void event_log_emit(event_log_code_t code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    evl_vemit(code, NULL, 0, fmt, ap);
    va_end(ap);
}

void event_log_emit_at(event_log_code_t code, const struct timeval *tv, int64_t up_ms,
                       const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    evl_vemit(code, tv, up_ms, fmt, ap);
    va_end(ap);
}

// ---- SD writer ----

static bool evl_sd_ready(void)
{
    // If main injected the real predicate use it; otherwise assume "maybe" and let fopen decide.
    return (s_sd_ready_fn != NULL) ? s_sd_ready_fn() : true;
}

// Rename ring: drop the oldest, shift events.i.log -> events.(i+1).log, active -> events.1.log.
// Must be called holding s_file_mtx with the active file CLOSED. Errors (ENOENT) are ignored.
static void evl_rotate(void)
{
    char a[64], b[64];
    snprintf(a, sizeof(a), EVENT_LOG_DIR "/events.%d.log", EVENT_LOG_KEEP_FILES);
    remove(a);
    for (int i = EVENT_LOG_KEEP_FILES - 1; i >= 1; i--)
    {
        snprintf(a, sizeof(a), EVENT_LOG_DIR "/events.%d.log", i);
        snprintf(b, sizeof(b), EVENT_LOG_DIR "/events.%d.log", i + 1);
        rename(a, b);
    }
    rename(EVENT_LOG_FILE, EVENT_LOG_DIR "/events.1.log");
    s_rotations++;
    s_file_bytes = 0;
    ESP_LOGI(TAG, "event log rotated (%u total)", (unsigned)s_rotations);
}

// Drain ring entries [persist..head) to the SD file. Bounded by mtx_wait_ms on the file mutex so a
// wedged card can never stall the caller (the reboot path passes a short timeout). Each line is
// copied out of the ring under the ring lock, then written outside it.
static void evl_drain(uint32_t mtx_wait_ms)
{
    // Snapshot the work window; if the ring wrapped past unpersisted entries, account the loss.
    portENTER_CRITICAL(&s_ring_lock);
    uint32_t head = s_head_seq;
    uint32_t persist = s_persist_seq;
    if ((head - persist) > EVENT_LOG_RING_N)
    {
        s_dropped += (head - persist) - EVENT_LOG_RING_N;
        persist = head - EVENT_LOG_RING_N;
        s_persist_seq = persist;
    }
    portEXIT_CRITICAL(&s_ring_lock);

    if (persist == head)
    {
        return;   // nothing pending
    }
    if (!evl_sd_ready())
    {
        return;   // no card: keep buffering in the ring
    }
    if (s_file_mtx == NULL ||
        xSemaphoreTake(s_file_mtx, pdMS_TO_TICKS(mtx_wait_ms)) != pdTRUE)
    {
        return;   // contended/unavailable: try again next tick
    }

    struct stat stx;
    if (stat(EVENT_LOG_DIR, &stx) != 0)
    {
        if (mkdir(EVENT_LOG_DIR, 0775) != 0)
        {
            s_sd_ok = false;
            xSemaphoreGive(s_file_mtx);
            return;
        }
    }

    FILE *f = fopen(EVENT_LOG_FILE, "a");
    if (f == NULL)
    {
        s_sd_ok = false;
        xSemaphoreGive(s_file_mtx);
        return;   // card vanished / FS error: leave persist where it is, retry later
    }

    bool wrote = true;
    for (uint32_t s = persist; s != head; s++)
    {
        char local[EVENT_LOG_LINE_MAX];
        bool valid;
        // Validate the slot still holds event s (seq == s+1). If a burst of emits overwrote it while
        // this (slow) SD write was in flight, the data is gone -- skip it (count the drop) instead of
        // writing a newer event's text into an older position, which would drop+duplicate lines.
        portENTER_CRITICAL(&s_ring_lock);
        valid = (s_ring[s % EVENT_LOG_RING_N].seq == s + 1);
        if (valid)
        {
            strlcpy(local, s_ring[s % EVENT_LOG_RING_N].line, sizeof(local));
        }
        portEXIT_CRITICAL(&s_ring_lock);

        if (!valid)
        {
            s_dropped++;
            continue;
        }
        if (fprintf(f, "%s\n", local) < 0)
        {
            wrote = false;
            break;
        }
    }
    fflush(f);
    fsync(fileno(f));
    long sz = ftell(f);
    fclose(f);

    if (wrote)
    {
        portENTER_CRITICAL(&s_ring_lock);
        s_persist_seq = head;
        portEXIT_CRITICAL(&s_ring_lock);
        s_sd_ok = true;
        if (sz >= 0) s_file_bytes = (uint32_t)sz;
        if (sz >= EVENT_LOG_ROTATE_BYTES)
        {
            evl_rotate();
        }
    }
    else
    {
        s_sd_ok = false;   // partial write: persist not advanced, lines re-sent next drain
    }

    xSemaphoreGive(s_file_mtx);
}

static void evl_writer_task(void *arg)
{
    (void)arg;
    int64_t start_us = esp_timer_get_time();
    bool guard_cleared = false;

    for (;;)
    {
        // Block until an event is signalled, or wake periodically as a flush heartbeat.
        if (s_wake != NULL)
        {
            xSemaphoreTake(s_wake, pdMS_TO_TICKS(EVENT_LOG_WAKE_MS));
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(EVENT_LOG_WAKE_MS));
        }

        evl_drain(1000);

        if (!guard_cleared && (esp_timer_get_time() - start_us) > EVENT_LOG_GUARD_STABLE_US)
        {
            s_evl_guard = 0;   // survived the danger window; future boots may retry SD persistence
            guard_cleared = true;
            ESP_LOGI(TAG, "event_log stable (15s) - crash guard cleared");
        }
    }
}

bool event_log_bringup_skipped(void)
{
    return s_bringup_skipped;
}

void event_log_init(void)
{
    if (s_inited)
    {
        return;
    }

    // One-shot crash guard: if a prior boot armed an attempt and didn't survive long enough to clear
    // it, that attempt crashed during event_log SD work -> skip the writer/SD this boot so a fault can
    // never boot-loop the device. The in-RAM ring still records everything; self-recovers next boot.
    if (s_evl_guard == EVENT_LOG_GUARD_MAGIC)
    {
        /* DISARM HERE, exactly as poll_log.c and fast_log.c do. The "self-recovers next boot"
         * promise above is only true because of this line: the guard's other clear point is the
         * writer task after 15 s of stability, and on a skip boot that task never starts. Leave
         * it armed and one crash inside the 15 s window costs SD event persistence permanently --
         * and, because resume-in-place refuses to resume while any bring-up was skipped, makes
         * EVERY wake take the reboot fallback forever while repairing nothing. Worse here than
         * elsewhere: the "resume refused -- rebooting" line would live only in the RAM ring and
         * die in the very reboot it announces, so the SD trail would show nothing at all. */
        s_evl_guard = 0;   /* disarm so the next boot retries */
        ESP_LOGW(TAG, "prior event_log attempt did not complete - SD persistence skipped this boot");
        s_inited = true;   // ring + emit still work; just no writer/SD this boot
        s_bringup_skipped = true;
        return;
    }
    s_evl_guard = EVENT_LOG_GUARD_MAGIC;   // arm

    s_wake = xSemaphoreCreateBinary();
    s_file_mtx = xSemaphoreCreateMutex();
    if (s_wake == NULL || s_file_mtx == NULL)
    {
        ESP_LOGE(TAG, "sync primitive alloc failed - SD persistence disabled (ring still active)");
        s_evl_guard = 0;
        s_inited = true;
        return;
    }

    // Writer task stack in INTERNAL RAM: it dereferences its stack locals inside fwrite/fsync
    // flash-cache-disable windows, where a PSRAM stack would fault (the csv_logger brick invariant).
    static StackType_t *evl_stack;
    static StaticTask_t evl_tcb;
    evl_stack = heap_caps_malloc(EVENT_LOG_WRITER_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (evl_stack == NULL)
    {
        ESP_LOGE(TAG, "writer stack alloc failed - SD persistence disabled (ring still active)");
        s_evl_guard = 0;
        s_inited = true;
        return;
    }

    if (xTaskCreateStatic(evl_writer_task, "event_log", EVENT_LOG_WRITER_STACK,
                          NULL, EVENT_LOG_WRITER_PRIO, evl_stack, &evl_tcb) == NULL)
    {
        ESP_LOGE(TAG, "writer task create failed - SD persistence disabled (ring still active)");
        heap_caps_free(evl_stack);
        s_evl_guard = 0;
        s_inited = true;
        return;
    }

    s_inited = true;
    ESP_LOGI(TAG, "event log started (ring=%d, file=%s)", EVENT_LOG_RING_N, EVENT_LOG_FILE);
}

// ---- HTTP retrieval (GET /event_log*) ----

// Stream the in-RAM ring oldest->newest as text/plain. Always works (no SD needed). Copies each line
// out under the ring lock, then sends it as a chunk outside the lock.
static esp_err_t evl_send_ram(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");

    uint32_t head, count;
    portENTER_CRITICAL(&s_ring_lock);
    head = s_head_seq;
    portEXIT_CRITICAL(&s_ring_lock);
    count = (head < EVENT_LOG_RING_N) ? head : EVENT_LOG_RING_N;

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t s = head - count + i;
        char local[EVENT_LOG_LINE_MAX + 2];
        bool valid;
        portENTER_CRITICAL(&s_ring_lock);
        valid = (s_ring[s % EVENT_LOG_RING_N].seq == s + 1);   // skip slots overwritten mid-dump
        if (valid)
        {
            strlcpy(local, s_ring[s % EVENT_LOG_RING_N].line, EVENT_LOG_LINE_MAX);
        }
        portEXIT_CRITICAL(&s_ring_lock);
        if (!valid)
        {
            continue;
        }
        strlcat(local, "\n", sizeof(local));
        if (httpd_resp_send_chunk(req, local, strlen(local)) != ESP_OK)
        {
            return ESP_FAIL;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// Stream the active SD log file chunked, using an OOM-safe laddered INTERNAL-RAM buffer (8K->1K),
// the same discipline sd_filemgr uses to survive WiFi+httpd memory pressure. Falls back to the RAM
// ring if the file can't be opened (no card / not yet written).
static esp_err_t evl_send_file(httpd_req_t *req)
{
    // Serialize against the writer task's append/rotate on the same file. A read handle held open
    // across the writer's rotation rename() is undefined on FatFs and can corrupt the directory
    // entry -- so take s_file_mtx for the whole fopen..fclose. If the writer is busy (e.g. mid-
    // rotation) and we can't acquire in time, fall back to the always-safe in-RAM ring.
    if (s_file_mtx == NULL || xSemaphoreTake(s_file_mtx, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        return evl_send_ram(req);
    }

    FILE *f = fopen(EVENT_LOG_FILE, "r");
    if (f == NULL)
    {
        xSemaphoreGive(s_file_mtx);
        return evl_send_ram(req);
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=events.log");

    size_t bufsz = 8 * 1024;
    char *buf = NULL;
    while (bufsz >= 1024)
    {
        buf = heap_caps_malloc(bufsz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (buf != NULL) break;
        bufsz /= 2;
    }
    if (buf == NULL)
    {
        fclose(f);
        xSemaphoreGive(s_file_mtx);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t n;
    esp_err_t ret = ESP_OK;
    while ((n = fread(buf, 1, bufsz, f)) > 0)
    {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
        {
            ret = ESP_FAIL;
            break;
        }
    }
    heap_caps_free(buf);
    fclose(f);
    xSemaphoreGive(s_file_mtx);
    httpd_resp_send_chunk(req, NULL, 0);
    return ret;
}

static esp_err_t evl_send_status(httpd_req_t *req)
{
    uint32_t head;
    portENTER_CRITICAL(&s_ring_lock);
    head = s_head_seq;
    portEXIT_CRITICAL(&s_ring_lock);
    uint32_t ring_count = (head < EVENT_LOG_RING_N) ? head : EVENT_LOG_RING_N;

    // Hand-rolled JSON (no cJSON dep -> keeps event_log a minimal leaf), mirroring poll_log.
    char body[256];
    snprintf(body, sizeof(body),
             "{\"sd_ready\":%s,\"sd_ok\":%s,\"file_bytes\":%u,\"rotations\":%u,"
             "\"events_total\":%u,\"ring_count\":%u,\"dropped\":%u,\"bad_ctx\":%u,"
             "\"debug\":%s}",
             evl_sd_ready() ? "true" : "false",
             s_sd_ok ? "true" : "false",
             (unsigned)s_file_bytes, (unsigned)s_rotations,
             (unsigned)head, (unsigned)ring_count, (unsigned)s_dropped,
             /* #111: non-zero means somebody emitted from a context that must not format --
              * sys_evt, esp_timer, or an ISR. See evl_caller_may_format(). */
             (unsigned)s_bad_ctx,
             /* #98: the ONLY way to confirm the debug-detail gate over WiFi on a device with no
              * readable serial console. */
             s_debug_events ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static esp_err_t evl_router_handler(httpd_req_t *req)
{
    const char *seg = req->uri + strlen("/event_log");
    if (*seg == '/') seg++;

    char route[16] = {0};
    size_t rl = strcspn(seg, "?");
    if (rl >= sizeof(route)) rl = sizeof(route) - 1;
    memcpy(route, seg, rl);
    route[rl] = '\0';

    if (strcmp(route, "status") == 0)
    {
        return evl_send_status(req);
    }
    if (strcmp(route, "ram") == 0)
    {
        return evl_send_ram(req);
    }
    // "" (the bare /event_log) or "tail" -> the persisted file, RAM-ring fallback.
    return evl_send_file(req);
}

esp_err_t event_log_register_handlers(httpd_handle_t server)
{
    static const httpd_uri_t evl_uri = {
        .uri = "/event_log*",
        .method = HTTP_GET,
        .handler = evl_router_handler,
        .user_ctx = NULL,
    };
    esp_err_t ret = httpd_register_uri_handler(server, &evl_uri);
    if (ret == ESP_OK || ret == ESP_ERR_HTTPD_HANDLER_EXISTS)
    {
        return ESP_OK;
    }
    return ret;
}
