/*
 * NC Flash fast flash -- the ESP side: ISO-TP over TWAI, the package in PSRAM, bus ownership, the
 * status lines, GET /fast_flash. The decisions (sequence, retries, rules) live in
 * fast_flash_logic.c; see ncflash_fastflash.h and docs/internals/fast_flash.md.
 *
 * Bus ownership is taken exactly like ncflash_fast_write(): single-owner check, FLASH_ACTIVE_BIT
 * (parks the datalogger, refuses MQTT/SLCAN producers, sleep, reboot, OTA and SD writes), the
 * #145 reboot/OTA fence, can_rx_task suspended, queues drained; released last on every exit.
 * ncflash_fastwrite.c is not touched: nothing here is shared with it.
 */
#include "ncflash_fastflash.h"
#include "fast_flash_logic.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/twai.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

#include "can.h"
#include "types.h"
#include "sdcard.h"
#include "event_log.h"
#include "config_server.h"   /* the #145 reboot/OTA fence */

#define TAG "ncflash_fastflash"

#define FF_TESTER_ID      0x7E0u
#define FF_ECU_ID         0x7E8u
#define FF_FRAME_SEND_MS  200      /* one CAN frame into the TWAI TX queue */
#define FF_FC_TIMEOUT_MS  2000     /* flow control after a first frame */
#define FF_FC_WAIT_MAX    16       /* "wait" flow controls tolerated per message */
#define FF_TXQ_BLOCK_MS   2000     /* before the point of no return: a PC this slow is gone */
/* Extra gap between consecutive frames, microseconds. 0 = back to back, as the kernel's flow control
 * asks (STmin 0). The kernel has ONE receive mailbox and drops a request whose frame was overwritten
 * (kernel.c can_poll); if a bench shows lost requests, a gap here is the knob. */
#ifndef NCFF_CF_GAP_US
#define NCFF_CF_GAP_US 0
#endif

#define FF_ROMS_DIR       SD_CARD_MOUNT_POINT "/roms"
#define FF_JOURNAL_PATH   FF_ROMS_DIR "/ff_journal.json"

/* ---- state shared with GET /fast_flash ------------------------------------------------------ */
static volatile int s_busy;
static SemaphoreHandle_t s_lock;
static struct {
    char        state[12];   /* idle running done failed interrupted */
    char        name[100];
    ff_status_t st;
    int         err_stage, err_nrc, err_blk;
    uint32_t    err_addr;
    bool        err_ponr;
    int64_t     t_start_us, t_end_us;
    uint32_t    lines_dropped;
    uint32_t    stack_free;      /* can_tx_task stack high-water mark after the last op, bytes */
    bool        journal_checked;
} s_info = { .state = "idle" };
static char s_result[900];
static char s_crcs[1400];

static void info_lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void info_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

/* ---- the I/O ops ---------------------------------------------------------------------------- */
typedef struct {
    QueueHandle_t *txq;
    xdev_buffer    out;
    uint32_t       dropped;
    char           name[100];
    char           mode;
} ff_io_t;

static uint32_t io_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void io_sleep_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

/* Next frame from the ECU before the absolute deadline. 0 = got one, -1 = deadline passed. */
static int io_rx_frame(twai_message_t *m, int64_t deadline_us)
{
    for (;;) {
        int64_t rem_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (rem_ms <= 0) return -1;
        TickType_t t = pdMS_TO_TICKS(rem_ms);
        if (can_receive(m, t ? t : 1) != ESP_OK) continue;
        if (m->identifier == FF_ECU_ID && !m->extd && !m->rtr && m->data_length_code >= 1) return 0;
    }
}

static void io_gap(uint8_t stmin)
{
    if (stmin >= 1 && stmin <= 0x7F) {
        TickType_t t = pdMS_TO_TICKS(stmin);
        vTaskDelay(t ? t : 1);
    } else if (stmin >= 0xF1 && stmin <= 0xF9) {
        esp_rom_delay_us(100u * (stmin - 0xF0u));
    }
    if (NCFF_CF_GAP_US > 0) esp_rom_delay_us(NCFF_CF_GAP_US);
}

/* One ISO-TP request to 0x7E0, honouring the ECU's flow control (BS and STmin). Frames from 0x7E8
 * that are not flow control (another tester's answer) are skipped while waiting for it. */
static int io_send(void *ctx, const uint8_t *p, uint32_t total)
{
    (void)ctx;
    twai_message_t tx = {0};
    tx.identifier = FF_TESTER_ID;
    tx.data_length_code = 8;
    if (total == 0 || total > 4095) return -FF_SUB_SEND;
    if (total <= 7) {
        tx.data[0] = (uint8_t)total;
        memcpy(&tx.data[1], p, total);
        return can_send(&tx, pdMS_TO_TICKS(FF_FRAME_SEND_MS)) == ESP_OK ? FF_IO_OK : -FF_SUB_SEND;
    }
    tx.data[0] = 0x10 | (uint8_t)((total >> 8) & 0x0F);
    tx.data[1] = (uint8_t)total;
    memcpy(&tx.data[2], p, 6);
    uint32_t idx = 6;
    if (can_send(&tx, pdMS_TO_TICKS(FF_FRAME_SEND_MS)) != ESP_OK) return -FF_SUB_SEND;
    uint8_t seq = 1;
    while (idx < total) {
        /* flow control */
        twai_message_t fc;
        int waits = 0;
        int64_t dl = esp_timer_get_time() + (int64_t)FF_FC_TIMEOUT_MS * 1000;
        for (;;) {
            if (io_rx_frame(&fc, dl) != 0) return -FF_SUB_FC_TO;
            if ((fc.data[0] >> 4) != 0x3) continue;            /* not flow control: not ours */
            uint8_t fs = fc.data[0] & 0x0F;
            if (fs == 0) break;                                /* clear to send */
            if (fs == 1 && ++waits <= FF_FC_WAIT_MAX) {        /* wait */
                dl = esp_timer_get_time() + (int64_t)FF_FC_TIMEOUT_MS * 1000;
                continue;
            }
            return -FF_SUB_FC_BAD;
        }
        uint8_t bs = fc.data[1], stmin = fc.data[2];
        uint32_t in_block = 0;
        while (idx < total) {
            twai_message_t cf = {0};
            cf.identifier = FF_TESTER_ID;
            cf.data_length_code = 8;
            cf.data[0] = 0x20 | (seq & 0x0F);
            for (int i = 1; i < 8; i++) cf.data[i] = idx < total ? p[idx++] : 0;
            if (can_send(&cf, pdMS_TO_TICKS(FF_FRAME_SEND_MS)) != ESP_OK) return -FF_SUB_CF_SEND;
            seq = (seq + 1) & 0x0F;
            if (idx < total) io_gap(stmin);
            if (bs && ++in_block == bs) break;                 /* next flow control */
        }
    }
    return FF_IO_OK;
}

/* The next single-frame answer from the ECU. Anything else from 0x7E8 is skipped. */
static int io_recv(void *ctx, uint8_t *buf, uint32_t cap, uint32_t *len, uint32_t timeout_ms)
{
    (void)ctx;
    int64_t dl = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    twai_message_t m;
    for (;;) {
        if (io_rx_frame(&m, dl) != 0) return FF_IO_TIMEOUT;
        if ((m.data[0] >> 4) != 0) continue;
        uint32_t n = m.data[0] & 0x0F;
        if (n < 1 || n > 7 || n + 1 > m.data_length_code) continue;
        if (n > cap) n = cap;
        memcpy(buf, &m.data[1], n);
        *len = n;
        return FF_IO_OK;
    }
}

static void io_flush(void *ctx)
{
    (void)ctx;
    twai_message_t junk;
    while (can_receive(&junk, 0) == ESP_OK) { /* drop late answers */ }
}

/* Before the point of no return a PC that does not take a line within FF_TXQ_BLOCK_MS is gone, and
 * the logic stops (the ECU's ROM is still untouched). Past it every line is a 0-tick try whose
 * failure is only counted: a lost PC can never stop a flash that has erased block 15. */
static int io_emit(void *ctx, const char *line, bool ponr)
{
    ff_io_t *io = (ff_io_t *)ctx;
    size_t n = strlen(line);
    if (n > DEV_BUFFER_LENGTH) n = DEV_BUFFER_LENGTH;
    io->out.usLen = (int)n;
    io->out.dev_channel = DEV_WIFI;
    memcpy(io->out.ucElement, line, n);
    ESP_LOGI(TAG, "%.*s", (int)(n && line[n - 1] == '\n' ? n - 1 : n), line);
    if (ponr) {
        if (xQueueSend(*io->txq, &io->out, 0) != pdTRUE) io->dropped++;
        return 0;
    }
    return xQueueSend(*io->txq, &io->out, pdMS_TO_TICKS(FF_TXQ_BLOCK_MS)) == pdTRUE ? 0 : -1;
}

/* The point of no return, on the SD card: after a dongle reset GET /fast_flash says "interrupted"
 * and names the package, so the PC knows to send mode R. Best effort; never fatal. */
static void io_persist(void *ctx, const char *event)
{
    ff_io_t *io = (ff_io_t *)ctx;
    FILE *f = fopen(FF_JOURNAL_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "journal not written (%s)", event);
        return;
    }
    fprintf(f, "{\"name\":\"%s\",\"mode\":\"%c\",\"event\":\"%s\"}", io->name, io->mode, event);
    fclose(f);
}

static void io_on_status(void *ctx, const ff_status_t *st)
{
    (void)ctx;
    info_lock();
    s_info.st = *st;
    info_unlock();
}

/* ---- helpers -------------------------------------------------------------------------------- */
static int ff_name_is_safe(const char *name)
{
    if (!name || name[0] == '\0' || name[0] == '.') return -1;
    size_t n = strlen(name);
    if (n > 96) return -1;
    int has_dot = 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (c == '.' && i + 1 < n && name[i + 1] == '.') return -1;
        if (c == '.') has_dot = 1;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.';
        if (!ok) return -1;
    }
    return has_dot ? 0 : -1;
}

/* Whole file into a buffer (PSRAM when psram). NULL on any failure; *len = size, or -1 if missing. */
static uint8_t *ff_read_file(const char *path, uint32_t max, bool psram, long *len)
{
    *len = -1;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    *len = sz;
    if (sz <= 0 || (uint32_t)sz > max) {
        fclose(f);
        return NULL;
    }
    uint8_t *b = psram ? heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                       : malloc((size_t)sz + 1);
    if (!b) {
        fclose(f);
        *len = -2;
        return NULL;
    }
    size_t got = 0;
    while (got < (size_t)sz) {
        size_t r = fread(b + got, 1, (size_t)sz - got, f);
        if (r == 0) break;
        got += r;
        if ((got & 0x3FFFF) == 0) vTaskDelay(1);
    }
    fclose(f);
    if (got != (size_t)sz) {
        free(b);
        *len = -3;
        return NULL;
    }
    b[sz] = 0;
    return b;
}

static void ff_write_result_file(const char *stem, const ff_t *ff)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s.result.json", FF_ROMS_DIR, stem);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "{\"result\":%s,\"crc\":%s}", ff->result_json[0] ? ff->result_json : "null",
            ff->crc_json[0] ? ff->crc_json : "null");
    fclose(f);
}

/* ---- the command ---------------------------------------------------------------------------- */
bool ncflash_fastflash_active(void)
{
    return s_busy != 0;
}

int ncflash_is_fastflash_cmd(const uint8_t *buf, int len)
{
    return len >= NCFLASH_FASTFLASH_MIN_LEN && buf[0] == NCFLASH_FASTFLASH_CMD;
}

int ncflash_fast_flash(const uint8_t *buf, int len, QueueHandle_t *tx_queue)
{
    if (!ncflash_is_fastflash_cmd(buf, len)) return -1;

    static ff_io_t io;   /* one op at a time (can_tx_task); kept off the 6 KB task stack */
    memset(&io, 0, sizeof(io));
    io.txq = tx_queue;
    io.mode = (char)buf[1];
    int nlen = 0;
    for (int i = 2; i < len && nlen < (int)sizeof(io.name) - 1; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') break;
        io.name[nlen++] = (char)buf[i];
    }
    io.name[nlen] = 0;

    const ff_ops_t ops = {
        .ctx = &io, .send = io_send, .recv = io_recv, .flush = io_flush, .now_ms = io_now_ms,
        .sleep_ms = io_sleep_ms, .emit = io_emit, .persist = io_persist, .on_status = io_on_status,
    };
    ff_t *ff = heap_caps_malloc(sizeof(ff_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ff) ff = malloc(sizeof(ff_t));
    if (!ff) {
        static const char oom[] = "\r\nFFERR a=000000 st=6 nrc=00 blk=0 ponr=0 app=u\r\n";
        (void)io_emit(&io, oom, false);
        return FF_ST_NOMEM;
    }
    ff_init(ff, &ops, io.mode, NCFF_ALLOW_LIVE != 0);
    /* A refusal before this op owns the bus must not overwrite the status of the previous op. */
    ff->ops.on_status = NULL;

    int rc;
    if (ff_name_is_safe(io.name) != 0 || !strchr("DCLR", io.mode) || io.mode == 0) {
        ESP_LOGE(TAG, "fast flash rejected: mode '%c' name '%s'", io.mode, io.name);
        ff_fail_early(ff, FF_ST_COMMAND, 0);
        free(ff);
        return FF_ST_COMMAND;
    }
    /* single bus owner: this op, the fast write and the fast read exclude each other */
    if (s_busy || can_flash_active()) {
        ESP_LOGW(TAG, "fast flash refused: another fast-op owns the bus");
        ff_fail_early(ff, FF_ST_BUSY, 0);
        free(ff);
        return FF_ST_BUSY;
    }
    s_busy = 1;
    can_flash_active_set();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!flash_fence_write_may_start(config_server_reboot_pending(), config_server_ota_active())) {
        can_flash_active_clear();
        s_busy = 0;
        ff_fail_early(ff, FF_ST_FENCE, 0);
        event_log_emit(EVL_WARN, "fast flash refused before it started (ECU not touched): the adapter is %s",
                       config_server_ota_active() ? "updating its firmware" : "about to reboot");
        free(ff);
        return FF_ST_FENCE;
    }

    info_lock();
    snprintf(s_info.state, sizeof(s_info.state), "running");
    snprintf(s_info.name, sizeof(s_info.name), "%s", io.name);
    memset(&s_info.st, 0, sizeof(s_info.st));
    s_info.st.mode = io.mode;
    s_info.err_stage = s_info.err_nrc = s_info.err_blk = 0;
    s_info.err_addr = 0;
    s_info.err_ponr = false;
    s_info.t_start_us = esp_timer_get_time();
    s_info.t_end_us = 0;
    s_info.lines_dropped = 0;
    s_info.journal_checked = true;   /* a journal from before this op is superseded */
    s_result[0] = s_crcs[0] = 0;
    info_unlock();
    ff->ops.on_status = io_on_status;

    uint8_t *pkg = NULL;
    char *json = NULL;
    char stem[100];
    snprintf(stem, sizeof(stem), "%s", io.name);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;

    TaskHandle_t rx_task = xTaskGetHandle("can_rx_task");
    if (rx_task) vTaskSuspend(rx_task);
    {
        twai_message_t junk;
        while (can_receive(&junk, 0) == ESP_OK) { }
        static xdev_buffer txjunk;   /* ~1 KB: kept off the 6 KB can_tx_task stack */
        while (xQueueReceive(*tx_queue, &txjunk, 0) == pdTRUE) { }
    }
    (void)io_emit(&io, "NCFFSYNC\n", false);
    ESP_LOGI(TAG, "fast flash %c %s (live %s)", io.mode, io.name, NCFF_ALLOW_LIVE ? "allowed" : "compiled out");

    char path[160];
    long l = 0;
    if (!sdcard_is_mounted()) {
        ff_fail_early(ff, FF_ST_NO_FILE, 0);
        rc = FF_ST_NO_FILE;
        goto cleanup;
    }
    snprintf(path, sizeof(path), "%s/%s.json", FF_ROMS_DIR, stem);
    json = (char *)ff_read_file(path, FF_MANIFEST_MAX, false, &l);
    if (!json) {
        ff_fail_early(ff, l == -2 ? FF_ST_NOMEM : FF_ST_MANIFEST, 0);
        rc = l == -2 ? FF_ST_NOMEM : FF_ST_MANIFEST;
        goto cleanup;
    }
    snprintf(path, sizeof(path), "%s/%s", FF_ROMS_DIR, io.name);
    pkg = ff_read_file(path, FF_PACKAGE_MAX, true, &l);
    if (!pkg) {
        rc = l == -1 ? FF_ST_NO_FILE : l == -2 ? FF_ST_NOMEM : l == -3 ? FF_ST_NO_FILE : FF_ST_SIZE;
        ff_fail_early(ff, rc, l > 0 ? (uint32_t)l : 0);
        goto cleanup;
    }
    rc = ff_check(ff, json, (size_t)strlen(json), pkg, (uint32_t)l);
    free(json);
    json = NULL;
    if (rc) goto cleanup;

    event_log_emit(EVL_FLASH_START, "fast %c %.48s", io.mode, io.name);
    /* From here nothing reads the SD card until the ECU is done. */
    rc = ff_run(ff);

cleanup:
    free(json);
    if (pkg) heap_caps_free(pkg);
    if (rx_task) vTaskResume(rx_task);
    {
        twai_message_t drain;
        while (can_receive(&drain, 0) == ESP_OK) { }
        uint32_t alerts = 0;
        (void)twai_read_alerts(&alerts, 0);
    }
    if (sdcard_is_mounted()) {
        ff_write_result_file(stem, ff);
        /* The journal says "this ECU may be waiting in its boot loader" across a dongle restart. It
         * goes once a live op settled that: success, or a failure that left the ROM untouched. */
        /* Never on an R failure (that ECU is the one the journal is about), never while this op knows
         * the ECU has no application (app=0 or u past a point of no return). */
        if (rc == 0 || (io.mode == 'L' && !ff->err_ponr && ff_app_state(ff) != '0')) unlink(FF_JOURNAL_PATH);
    }
    info_lock();
    snprintf(s_info.state, sizeof(s_info.state), rc == 0 ? "done" : "failed");
    s_info.st = ff->st;
    s_info.err_stage = ff->err_stage;
    s_info.err_nrc = ff->err_nrc;
    s_info.err_blk = ff->err_blk;
    s_info.err_addr = ff->err_addr;
    s_info.err_ponr = ff->err_ponr;
    s_info.t_end_us = esp_timer_get_time();
    s_info.lines_dropped = io.dropped;
    /* can_tx_task has 6 KB and no serial console to watch it: the headroom goes to GET /fast_flash */
    s_info.stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    snprintf(s_result, sizeof(s_result), "%s", ff->result_json);
    snprintf(s_crcs, sizeof(s_crcs), "%s", ff->crc_json);
    info_unlock();

    if (io.dropped)
        event_log_emit(EVL_INFO, "fast flash: the PC tool stopped listening (%lu lines lost) -- the flash %s without it",
                       (unsigned long)io.dropped, rc == 0 ? "finished" : "failed");
    if (rc == 0)
        event_log_emit(EVL_FLASH_OK, "fast %c rewritten=%lu nothing=%d elapsed=%lums name=%.40s", io.mode,
                       (unsigned long)ff->st.done, ff->nothing_to_do ? 1 : 0, (unsigned long)ff->ms_total, io.name);
    else
        event_log_emit(EVL_FLASH_FAIL, "fast %c st=%d nrc=0x%02X blk=%d ponr=%d a=%06lX name=%.40s", io.mode, rc,
                       ff->err_nrc, ff->err_blk, ff->err_ponr ? 1 : 0, (unsigned long)ff->err_addr, io.name);
    /* Release the bus LAST: only now does the datalogger un-park. */
    can_flash_active_clear();
    s_busy = 0;
    free(ff);
    return rc;
}

/* ---- GET /fast_flash ------------------------------------------------------------------------ */
/* A journal left on the SD card means the dongle reset after an op's point of no return, or an op
 * failed past it: the ECU may be waiting in its boot loader. Read once after boot, while no op runs. */
static void ff_check_journal(void)
{
    if (s_info.journal_checked || s_busy || !sdcard_is_mounted()) return;
    s_info.journal_checked = true;
    long l = 0;
    char *j = (char *)ff_read_file(FF_JOURNAL_PATH, 512, false, &l);
    if (!j) return;
    snprintf(s_info.state, sizeof(s_info.state), "interrupted");
    const char *n = strstr(j, "\"name\":\"");
    if (n) {
        n += 8;
        size_t k = 0;
        while (n[k] && n[k] != '"' && k < sizeof(s_info.name) - 1) {
            s_info.name[k] = n[k];
            k++;
        }
        s_info.name[k] = 0;
    }
    s_info.st.ponr = true;
    free(j);
}

static esp_err_t fast_flash_get_handler(httpd_req_t *req)
{
    char *out = malloc(4096);
    if (!out) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    info_lock();
    ff_check_journal();
    int64_t now = esp_timer_get_time();
    int64_t el = s_info.t_start_us ? ((s_info.t_end_us ? s_info.t_end_us : now) - s_info.t_start_us) / 1000 : 0;
    char mode[2] = { s_info.st.mode ? s_info.st.mode : '-', 0 };
    snprintf(out, 4096,
             "{\"rev\":%d,\"live_allowed\":%d,\"state\":\"%s\",\"name\":\"%s\",\"mode\":\"%s\",\"phase\":\"%s\","
             "\"blk\":%d,\"done\":%lu,\"total\":%lu,\"ponr\":%d,\"elapsed_ms\":%lld,\"lines_dropped\":%lu,"
             "\"stack_free\":%lu,"
             "\"err\":{\"st\":%d,\"nrc\":%d,\"a\":%lu,\"blk\":%d,\"ponr\":%d},\"result\":%s,\"crc\":%s}",
             NCFLASH_FASTFLASH_REV, NCFF_ALLOW_LIVE ? 1 : 0, s_info.state, s_info.name, mode, s_info.st.phase,
             s_info.st.blk, (unsigned long)s_info.st.done, (unsigned long)s_info.st.total, s_info.st.ponr ? 1 : 0,
             (long long)el, (unsigned long)s_info.lines_dropped, (unsigned long)s_info.stack_free, s_info.err_stage, s_info.err_nrc,
             (unsigned long)s_info.err_addr, s_info.err_blk, s_info.err_ponr ? 1 : 0,
             s_result[0] ? s_result : "null", s_crcs[0] ? s_crcs : "null");
    info_unlock();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static const httpd_uri_t fast_flash_uri = {
    .uri = "/fast_flash",
    .method = HTTP_GET,
    .handler = fast_flash_get_handler,
    .user_ctx = NULL,
};

int ncflash_fastflash_register_handlers(httpd_handle_t server)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    esp_err_t r = httpd_register_uri_handler(server, &fast_flash_uri);
    if (r != ESP_OK) ESP_LOGE(TAG, "GET /fast_flash not registered: %s", esp_err_to_name(r));
    return r == ESP_OK ? 1 : 0;
}
