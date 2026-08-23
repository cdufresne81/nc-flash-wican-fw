/*
 * NC Flash fast ROM WRITE — autonomous in-firmware SD-staged flash (Option B).
 * See ncflash_fastwrite.h for the rationale, wire protocol, and safety model.
 */

#include "ncflash_fastwrite.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/twai.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"

#include "can.h"
#include "types.h"
#include "sdcard.h"
#include "event_log.h"

#define TAG "ncflash_fastwrite"

/* ===== HARD SAFETY GATE =====================================================
 * 0 = DRY-RUN ONLY: a 'W' live ('L') command is REFUSED. The firmware can only
 *     verify the digest, walk the SD blocks, and stream progress — it NEVER
 *     issues RequestDownload/TransferData, so it cannot erase or write the ECU.
 * 1 = LIVE ENABLED (Phase 5 build, recoverable ECU only).
 * Keep this 0 for the Phase 4 dry-run build. */
#define NCFW_ALLOW_LIVE 1

#define TESTER_ID 0x7E0u
#define ECU_ID 0x7E8u
#define FRAME_TIMEOUT_MS 200
#define FC_TIMEOUT_MS 2000        /* ECU may be slow to Flow-Control while busy (e.g. SBL init/erase) */
#define RESP_FIRST_TIMEOUT_MS 5000 /* ordinary mid-region block: keep TIGHT, see below */
#define RESP_PENDING_TIMEOUT_MS 5000
/* The ECU erases its application region as soon as the SBL starts running, and it can answer
 * NOTHING at all while it does -- an erase here takes well over 5 s. RESP_FIRST_TIMEOUT_MS used to
 * cover that case too, and it does not: on 2026-08-23 a real flash timed out on the LAST SBL block
 * with FWSUB_ACK_TO and walked away from an ECU that was mid-erase (issue #126).
 *
 * Aborting there does not un-erase anything. It abandons an ECU that would have finished, leaving
 * it with no valid application until someone re-flashes it. So at the erase edge the safe move is
 * to WAIT, and the ceiling exists only so a truly dead ECU cannot hang the task forever.
 *
 * Deliberately asymmetric with RESP_FIRST_TIMEOUT_MS: mid-region a missing ACK really is fatal
 * (TransferData carries no sequence counter, so there is no resend -- see the header), and failing
 * fast is correct there. Only the erase edge gets the long ceiling.
 *
 * 60 s is not a guess: the legacy host path gave every request a cumulative 60 s silent budget
 * (TIMEOUT_RESPONSE_PENDING_MAX, nc-flash src/ecu/constants.py:158, commented "generous, to ride
 * out a slow flash erase"). And it is now backed by measurement: a full-ROM erase on a live NC
 * PCM took 12.6 s (bench, 2026-08-23, logged by the emit below), so 60 s is ~4.7x headroom rather
 * than a round number. Do not shrink it toward the measurement -- a colder or older ECU has no
 * reason to match, and the cost of being wrong is a bricked one.
 *
 * This is ONE budget for the WHOLE edge: the Flow-Control wait and the ACK wait share it. They are
 * two separate stalls and a silently-erasing ECU can hit either, so budgeting them separately
 * would allow 60+60 s of firmware silence and re-open the host race below. The deadline is taken
 * once, before the block is sent, and both waits count against it.
 *
 * This ceiling is NO LONGER coupled to any host version. It used to be: the host gives up after
 * _FAST_WRITE_IDLE_MS of silence (nc-flash src/ecu/wican_transport.py), closes the socket, our
 * next progress write fails, and the flash aborted via host_gone -- abandoning the ECU right
 * after the erase. That constant is 30 s in NC Flash 2.12.0 and older, the same as our old
 * ceiling, so the old host ALWAYS fired first. The KEEPALIVE below now feeds the host during the
 * stall, so its idle clock never expires however long the ECU erases, on EVERY host version.
 * This number is therefore sized on ECU evidence alone: "how long before a silent ECU is dead",
 * 60 s against a measured 12.6 s erase.
 *
 * The one host-side wall a keepalive does NOT push back is the host's absolute wall-clock budget
 * for the whole write (FAST_WRITE_TIMEOUT_MS = 600 s, wican_transport.py:185), which no traffic
 * resets. Transfer plus 60 s per edge plus 60 s at TransferExit is comfortably inside it -- but
 * anyone growing these ceilings must re-check that. */
#define RESP_ERASE_TIMEOUT_MS 60000

/* Heartbeat sent while we are waiting on a silently-erasing ECU (#126 follow-up).
 *
 * The binding constraint is NC Flash 2.12.0 and older, which declare the firmware dead after
 * 30 s with no bytes (_FAST_WRITE_IDLE_MS = 30000, nc-flash src/ecu/wican_transport.py:189 at
 * 575b3c0~1; 2.13.0 raised it to 90 s). The host resets that clock on ANY bytes received, before
 * it parses anything, so a small line every 5 s keeps every host version alive -- 6x margin, so
 * five consecutive beats can be lost or delayed (host GC, its 1 s select granularity, a WiFi
 * retry burst) before the oldest host gives up.
 *
 * Do not drift this upward toward 30 s: the margin is there to absorb exactly the hiccups we
 * cannot schedule. The assert names the old host constant so nobody can. */
#define KEEPALIVE_MS 5000
_Static_assert(3 * KEEPALIVE_MS < 30000,
               "keepalive must beat NC Flash 2.12.0's 30 s _FAST_WRITE_IDLE_MS several times over");
#define MAX_PENDING 24            /* ~ host TIMEOUT_RESPONSE_PENDING_MAX budget for 0x78 retries */

/* Granular ISO-TP sub-failure codes surfaced in the FWERR nrc field so a flash
 * abort says exactly WHERE a block died (vs the old opaque 0xFF). */
#define FWSUB_FF_SEND  0xE1 /* First Frame can_send failed */
#define FWSUB_FC_TO    0xE2 /* no Flow Control from ECU (recv timeout) */
#define FWSUB_FC_BAD   0xE3 /* Flow Control was not Clear-To-Send */
#define FWSUB_CF_SEND  0xE4 /* a Consecutive Frame can_send failed */
#define FWSUB_ACK_TO   0xE5 /* no response after the block (ECU silent) */
#define FWSUB_ACK_PCI  0xE6 /* response was not a single-frame */
#define FWSUB_ACK_SID  0xE7 /* response SID was neither the expected positive nor an NRC */
#define TX_QUEUE_SEND_TIMEOUT_MS 2000 /* host-gone abort threshold (never block forever) */
#define PROG_EVERY_N 16               /* stream NCFWPROG every N blocks */
#define MAX_MANIFEST_BYTES 8192

#define UDS_NRC 0x7F
#define NRC_RESPONSE_PENDING 0x78

#define FW_ROMS_DIR SD_CARD_MOUNT_POINT "/roms"

/* Streaming + block scratch (one fast-op at a time, in can_tx_task; static keeps
 * it off the task stack). s_fw_msg holds the ISO-TP payload: [0x36] + one block. */
static xdev_buffer s_out;
static uint8_t s_fw_chunk[4096];          /* CRC32 file reader */
static uint8_t s_fw_msg[1 + 1024 + 8];    /* 0x36 + up to a 1 KB block */

/* Re-entry guard: a fast-op owns the CAN bus exclusively. */
static volatile int s_fwbusy;

/* Last fw_emit_err() coordinates, stashed so the cleanup path can name in EVL_FLASH_FAIL
 * exactly WHERE a flash died (the stage + the FWSUB_* or NRC sub-code) without threading them
 * through every goto site. Reset at the top of each flash op. */
static int s_fw_err_stage;
static int s_fw_err_nrc;

/* POINT OF NO RETURN (#126 follow-up). Set the instant the ECU can have started erasing -- just
 * before the LAST SBL block goes out -- and cleared only at the top of the next op.
 *
 * Past this line the ECU has no valid application until we finish, and NOTHING the host does may
 * make us walk away: the host is an observer that cannot abort a flash (see the fast_write
 * docstring in nc-flash src/ecu/wican_transport.py), the image, manifest and CRC all live on the
 * SD card, so the firmware can and must finish alone. Before it, an emit failure still aborts,
 * which is correct -- the ECU's application is untouched until the SBL runs, and stopping there
 * costs nothing.
 *
 * Enforced centrally in tx_send() rather than at each call site, so an emit added later inherits
 * the rule instead of having to remember it. */
static volatile int s_ponr;

/* Keepalive context. One task owns a fast-op (can_tx_task, guarded by s_fwbusy), so no locking.
 * A file-scope struct rather than a parameter threaded through fw_isotp_send / fw_await_positive
 * / fw_transfer_data: same behaviour, four fewer signature changes in ECU-touching code -- the
 * same reasoning as the s_fw_err_* stash above. */
static struct {
    bool armed;
    QueueHandle_t *tx_queue;
    int64_t next_beat_us;
    uint32_t done, total;   /* numbers the beat reports; done tracks the block loop */
    uint32_t dropped;       /* lines the host could not be given -- counted, never fatal */
    uint32_t drop_blk;      /* block reached when the FIRST line went undelivered */
} s_ka;

/* One undelivered line. Records where the host stopped listening the first time, which is the
 * number worth reporting -- by cleanup the block counter has run on to the end. */
static void fw_ka_note_drop(void)
{
    if (s_ka.dropped == 0) s_ka.drop_blk = s_ka.done;
    s_ka.dropped++;
}

typedef struct {
    int manifest_version;
    uint32_t download_addr, download_size, block_size;
    uint32_t sbl_offset, sbl_len, program_offset, program_len;
    uint32_t image_len, image_crc32;
} fw_manifest_t;

/* ---- small helpers --------------------------------------------------------*/

/* zlib-compatible reflected CRC-32 step (matches host wican_sd_package.crc32). */
static uint32_t fw_crc32_step(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

/* Bounded send of s_out to the host TCP queue (the wedge-safe primitive: never
 * portMAX_DELAY — a host disconnect must time out into the clean teardown). */
static int tx_send(QueueHandle_t *tx_queue)
{
    /* Past the point of no return every line becomes a 0-tick try-send whose failure is counted
     * and IGNORED -- it returns 0, so the existing
     *     if (fw_emit(...) != 0) { host_gone = 1; rc = -3; goto cleanup; }
     * sites become no-ops instead of abandoning a freshly-erased ECU. This is the hole that
     * existed with matched versions too: a 2 s WiFi stall on any post-erase progress line used to
     * brick the ECU.
     *
     * 0 ticks, not a shorter block: with the drain task alive, a full queue means the socket has
     * not accepted a line in ~32 tries, so the host is unreachable and the bytes would not land
     * anyway -- while blocking inside an armed wait would eat the ECU's erase budget. The 32-deep
     * queue is itself the tolerance for ordinary WiFi jitter. */
    if (s_ponr)
    {
        if (xQueueSend(*tx_queue, &s_out, 0) != pdTRUE) fw_ka_note_drop();
        return 0;
    }
    return (xQueueSend(*tx_queue, &s_out, pdMS_TO_TICKS(TX_QUEUE_SEND_TIMEOUT_MS)) == pdTRUE)
               ? 0
               : -1;
}

/* Stream a NUL-terminated ASCII line. Returns 0 on success, -1 if host gone. */
static int fw_emit(QueueHandle_t *tx_queue, const char *line)
{
    size_t n = strlen(line);
    if (n > DEV_BUFFER_LENGTH) n = DEV_BUFFER_LENGTH;
    s_out.usLen = (int)n;
    s_out.dev_channel = DEV_WIFI;
    memcpy(s_out.ucElement, line, n);
    return tx_send(tx_queue);
}

/* Stream a terminal FWERR diagnostic (best-effort; host may already be gone). */
static void fw_emit_err(QueueHandle_t *tx_queue, uint32_t addr, int stage, int nrc)
{
    s_fw_err_stage = stage;   /* remember the failure site for EVL_FLASH_FAIL at cleanup */
    s_fw_err_nrc = nrc;
    char line[64];
    int n = snprintf(line, sizeof(line), "\r\nFWERR a=%06lX st=%d nrc=%02X\r\n",
                     (unsigned long)addr, stage, nrc & 0xFF);
    if (n > 0)
    {
        s_out.usLen = n;
        s_out.dev_channel = DEV_WIFI;
        memcpy(s_out.ucElement, line, (size_t)n);
        (void)tx_send(tx_queue);
    }
    ESP_LOGE(TAG, "FWERR a=%06lX st=%d nrc=%02X", (unsigned long)addr, stage, nrc & 0xFF);
}

/* ---- erase-edge keepalive -------------------------------------------------*/

/* Arm the heartbeat for one wait on a possibly-erasing ECU. `done`/`total` are the numbers the
 * beat reports; they do not move while a single block is in flight. */
static void fw_ka_arm(QueueHandle_t *tx_queue, uint32_t done, uint32_t total)
{
    s_ka.tx_queue     = tx_queue;
    s_ka.done         = done;
    s_ka.total        = total;
    s_ka.next_beat_us = esp_timer_get_time() + (int64_t)KEEPALIVE_MS * 1000;
    s_ka.armed        = true;
}

static void fw_ka_disarm(void)
{
    s_ka.armed = false;
}

/* One beat if one is due. Called from inside the receive loop.
 *
 * The line is a plain repeat of NCFWPROG, deliberately: every deployed host parses an unknown
 * line by ignoring it, so a new marker word would be safe but would buy nothing -- while
 * NCFWPROG additionally re-feeds the host's progress callback, so the user's progress bar stays
 * visibly alive through the stall instead of freezing. A repeated done/total is harmless (the
 * host re-reports the same count; a malformed one is swallowed by its own except).
 *
 * It MUST NOT start with FWERR or NCFWDONE -- both are terminal at the host, so a beat carrying
 * either prefix would end the very session it exists to keep alive.
 *
 * Never blocks: 0-tick send, failure counted only. Nothing inside an armed wait may block on
 * anything except can_receive(), or the ECU loses erase budget to it. */
static void fw_ka_tick(void)
{
    if (!s_ka.armed) return;
    int64_t now = esp_timer_get_time();
    if (now < s_ka.next_beat_us) return;
    /* Next beat measured from NOW, so a late one never bursts to catch up. */
    s_ka.next_beat_us = now + (int64_t)KEEPALIVE_MS * 1000;

    char line[40];
    int n = snprintf(line, sizeof(line), "NCFWPROG %lu/%lu\n",
                     (unsigned long)s_ka.done, (unsigned long)s_ka.total);
    if (n <= 0) return;
    if (n > (int)DEV_BUFFER_LENGTH) n = DEV_BUFFER_LENGTH;
    s_out.usLen = n;
    s_out.dev_channel = DEV_WIFI;
    memcpy(s_out.ucElement, line, (size_t)n);
    if (xQueueSend(*s_ka.tx_queue, &s_out, 0) != pdTRUE) fw_ka_note_drop();
}

/* Reject anything but a simple leaf filename with an extension. */
static int fw_name_is_safe(const char *name)
{
    if (!name || name[0] == '\0' || name[0] == '.') return -1;
    size_t n = strlen(name);
    if (n > 96) return -1;
    int has_dot = 0;
    for (size_t i = 0; i < n; i++)
    {
        char c = name[i];
        if (c == '/' || c == '\\') return -1;
        if (c == '.' && i + 1 < n && name[i + 1] == '.') return -1;
        if (c == '.') has_dot = 1;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return -1;
    }
    return has_dot ? 0 : -1;
}

static int fw_mf_u32(cJSON *root, const char *key, uint32_t *out)
{
    cJSON *it = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(it)) return -1;
    *out = (uint32_t)it->valuedouble; /* valuedouble is exact for our <2^32 ints */
    return 0;
}

static int fw_load_manifest(const char *path, fw_manifest_t *m)
{
    FILE *jf = fopen(path, "rb");
    if (!jf) return -1;
    fseek(jf, 0, SEEK_END);
    long sz = ftell(jf);
    fseek(jf, 0, SEEK_SET);
    if (sz <= 0 || sz > MAX_MANIFEST_BYTES) { fclose(jf); return -1; }
    char *txt = malloc((size_t)sz + 1);
    if (!txt) { fclose(jf); return -1; }
    size_t rd = fread(txt, 1, (size_t)sz, jf);
    fclose(jf);
    txt[rd] = '\0';
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) return -1;

    cJSON *mv = cJSON_GetObjectItem(root, "manifest_version");
    m->manifest_version = cJSON_IsNumber(mv) ? mv->valueint : -1;
    int bad = 0;
    bad |= fw_mf_u32(root, "download_addr", &m->download_addr);
    bad |= fw_mf_u32(root, "download_size", &m->download_size);
    bad |= fw_mf_u32(root, "block_size", &m->block_size);
    bad |= fw_mf_u32(root, "sbl_offset", &m->sbl_offset);
    bad |= fw_mf_u32(root, "sbl_len", &m->sbl_len);
    bad |= fw_mf_u32(root, "program_offset", &m->program_offset);
    bad |= fw_mf_u32(root, "program_len", &m->program_len);
    bad |= fw_mf_u32(root, "image_len", &m->image_len);
    bad |= fw_mf_u32(root, "image_crc32", &m->image_crc32);
    cJSON_Delete(root);
    return bad ? -1 : 0;
}

/* ---- LIVE-mode CAN/UDS primitives (compiled + reachable ONLY when a real flash
 *      is permitted; in a dry-run build mode 'L' is rejected before any of these
 *      can run, so the ECU is never contacted). --------------------------------*/

static int recv_matching(twai_message_t *msg, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline)
    {
        int64_t rem_ms = (deadline - esp_timer_get_time()) / 1000;
        if (rem_ms < 1) rem_ms = 1;
        /* Keepalive slicing (#126 follow-up). UNARMED -- every ordinary wait, the whole
         * fast-read path, and all of dry-run -- fw_ka_tick() returns at once and rem_ms is
         * exactly what it was before, so those paths are unchanged.
         *
         * Armed, the wait is capped at the next beat so the heartbeat can go out mid-erase. This
         * cannot lose or delay the ECU's answer: a frame arriving on a slice boundary sits in the
         * TWAI driver's RX queue (96-100 deep, can.c:123) and is returned by the very next
         * can_receive(); and `deadline` is an absolute esp_timer value, so slicing cannot drift
         * the edge budget. */
        fw_ka_tick();
        if (s_ka.armed)
        {
            int64_t beat_ms = (s_ka.next_beat_us - esp_timer_get_time()) / 1000;
            if (beat_ms < 1) beat_ms = 1;
            if (beat_ms < rem_ms) rem_ms = beat_ms;
        }
        if (can_receive(msg, pdMS_TO_TICKS(rem_ms)) == ESP_OK)
            if (msg->identifier == ECU_ID && msg->rtr == 0) return 0;
    }
    return -1;
}

/* Send an ISO-TP message (single- or multi-frame) of `total` payload bytes,
 * honoring the ECU's Flow Control. NO resend on error. Returns 0 on success or a
 * negative FWSUB_* sub-code so the caller can report exactly where it failed. */
/* Milliseconds left until an absolute deadline. deadline_us == 0 means "no edge deadline in
 * force", so the caller's ordinary timeout applies unchanged. Past the deadline this returns 1
 * rather than 0 so the recv fails naturally on its next pass instead of needing a second exit
 * path. */
static int fw_ms_left(int64_t deadline_us, int fallback_ms)
{
    if (deadline_us == 0) return fallback_ms;
    int64_t left = (deadline_us - esp_timer_get_time()) / 1000;
    return (left < 1) ? 1 : (int)left;
}

static int fw_isotp_send(const uint8_t *payload, uint32_t total, int64_t deadline_us)
{
    twai_message_t tx = {0};
    tx.identifier = TESTER_ID;
    tx.extd = 0;
    tx.data_length_code = 8;

    if (total <= 7)
    {
        tx.data[0] = (uint8_t)total; /* Single Frame */
        for (uint32_t i = 0; i < total; i++) tx.data[1 + i] = payload[i];
        return (can_send(&tx, pdMS_TO_TICKS(FRAME_TIMEOUT_MS)) == ESP_OK) ? 0 : -FWSUB_FF_SEND;
    }

    /* First Frame */
    tx.data[0] = 0x10 | (uint8_t)((total >> 8) & 0x0F);
    tx.data[1] = (uint8_t)(total & 0xFF);
    uint32_t idx = 0;
    for (int i = 2; i < 8; i++) tx.data[i] = (idx < total) ? payload[idx++] : 0;
    if (can_send(&tx, pdMS_TO_TICKS(FRAME_TIMEOUT_MS)) != ESP_OK) return -FWSUB_FF_SEND;

    /* Flow Control (expect Clear-To-Send 0x30; BS/STmin honored). */
    twai_message_t fc;
    /* An ECU too busy erasing to ACK is also too busy to Flow-Control, and that stall lands HERE,
     * before fw_await_positive ever runs. Without the deadline this aborts at FC_TIMEOUT_MS = 2 s
     * and the long ACK ceiling never applies -- which made the "first program block" edge a false
     * promise. */
    if (recv_matching(&fc, fw_ms_left(deadline_us, FC_TIMEOUT_MS)) != 0) return -FWSUB_FC_TO;
    if ((fc.data[0] & 0xF0) != 0x30 || (fc.data[0] & 0x0F) != 0x00) return -FWSUB_FC_BAD;
    uint8_t stmin = fc.data[2];

    uint8_t seq = 1;
    while (idx < total)
    {
        twai_message_t cf = {0};
        cf.identifier = TESTER_ID;
        cf.extd = 0;
        cf.data_length_code = 8;
        cf.data[0] = 0x20 | (seq & 0x0F);
        for (int i = 1; i < 8; i++) cf.data[i] = (idx < total) ? payload[idx++] : 0;
        if (can_send(&cf, pdMS_TO_TICKS(FRAME_TIMEOUT_MS)) != ESP_OK) return -FWSUB_CF_SEND;
        seq = (seq + 1) & 0x0F;
        if (stmin > 0 && stmin <= 0x7F) vTaskDelay(pdMS_TO_TICKS(stmin)); /* ms range */
    }
    return 0;
}

/* Await a positive UDS response with SID `expect`, riding out 7F xx 78. Sets
 * *out_nrc to the real ECU NRC, or an FWSUB_ACK_* code, on failure. */
static int fw_await_positive(uint8_t expect, int *out_nrc, int64_t deadline_us)
{
    int pending = 0;
    for (;;)
    {
        twai_message_t rx;
        /* With an edge deadline in force this also caps the PENDING path. Without it, one 0x78
         * followed by a silent erase would drop back to RESP_PENDING_TIMEOUT_MS and abort at 5 s
         * despite the long ceiling -- while 24 pendings at the long value would blow the host 600 s
         * total the other way. The deadline bounds both ends. */
        int to = fw_ms_left(deadline_us, pending ? RESP_PENDING_TIMEOUT_MS : RESP_FIRST_TIMEOUT_MS);
        if (recv_matching(&rx, to) != 0) { *out_nrc = FWSUB_ACK_TO; return -1; }
        if ((rx.data[0] >> 4) != 0x0) { *out_nrc = FWSUB_ACK_PCI; return -1; } /* want SF reply */
        uint8_t l = rx.data[0] & 0x0F;
        if (l >= 3 && rx.data[1] == UDS_NRC && rx.data[3] == NRC_RESPONSE_PENDING)
        {
            if (++pending > MAX_PENDING) { *out_nrc = NRC_RESPONSE_PENDING; return -1; }
            continue;
        }
        if (l >= 1 && rx.data[1] == expect) return 0;             /* positive */
        if (l >= 3 && rx.data[1] == UDS_NRC) { *out_nrc = rx.data[3]; return -1; }
        *out_nrc = FWSUB_ACK_SID;
        return -1;
    }
}

/* RequestDownload [0x34]+addr(4BE)+size(4BE) (KWP2000-style, no ALFID). */
static int fw_request_download(uint32_t addr, uint32_t size, int *nrc)
{
    uint8_t p[9] = {0x34,
                    (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
                    (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size};
    int s = fw_isotp_send(p, sizeof(p), 0);
    if (s != 0) { *nrc = -s; return -1; }
    /* Ordinary timeout is correct here: the SBL is the code that erases and it is delivered by
     * TransferData AFTER this, so nothing can be erasing yet -- and a 0x34 failure is pre-erase,
     * hence safely retryable. */
    return fw_await_positive(0x74, nrc, 0);
}

/* TransferData [0x36]+block (NO sequence counter), then await 0x76. s_fw_msg[0]
 * is preset to 0x36 by the caller; block bytes live at s_fw_msg[1..blen]. */
static int fw_transfer_data(uint32_t blen, int *nrc, int64_t deadline_us)
{
    s_fw_msg[0] = 0x36;
    int s = fw_isotp_send(s_fw_msg, blen + 1, deadline_us);
    if (s != 0) { *nrc = -s; return -1; }
    return fw_await_positive(0x76, nrc, deadline_us);
}

/* TransferExit also gets the long ceiling: the ECU may verify or finalise here, and a 5 s timeout
 * on a finalising ECU declares failure, skips the ECU reset, and pushes the user into a needless
 * extra flash cycle of a probably-fine ECU. The legacy host gave 0x37 the same 60 s budget as
 * everything else. Costs nothing when the ECU answers promptly. */
static int fw_transfer_exit(int *nrc)
{
    uint8_t p[1] = {0x37};
    const int64_t deadline = esp_timer_get_time() + (int64_t)RESP_ERASE_TIMEOUT_MS * 1000;
    int s = fw_isotp_send(p, 1, deadline);
    if (s != 0) { *nrc = -s; return -1; }
    return fw_await_positive(0x77, nrc, deadline);
}

static void fw_ecu_reset(void)
{
    uint8_t p[2] = {0x11, 0x01};
    (void)fw_isotp_send(p, 2, 0); /* best-effort; no/late response is expected */
}

/* ---- command entry --------------------------------------------------------*/

int ncflash_is_fastwrite_cmd(const uint8_t *buf, int len)
{
    return (len >= NCFLASH_FASTWRITE_MIN_LEN && buf[0] == NCFLASH_FASTWRITE_CMD);
}

int ncflash_fast_write(const uint8_t *buf, int len, QueueHandle_t *tx_queue)
{
    if (!ncflash_is_fastwrite_cmd(buf, len)) return -1;

    char mode = (char)buf[1];

    /* Parse the staged leaf filename (buf[2..] up to CR/LF). */
    char name[97];
    int nlen = 0;
    for (int i = 2; i < len && nlen < (int)sizeof(name) - 1; i++)
    {
        uint8_t c = buf[i];
        if (c == '\r' || c == '\n') break;
        name[nlen++] = (char)c;
    }
    name[nlen] = '\0';
    if (fw_name_is_safe(name) != 0)
    {
        ESP_LOGE(TAG, "fast write rejected: unsafe name '%s'", name);
        fw_emit_err(tx_queue, 0, 8, 0);
        return -1;
    }

    /* Unified single-CAN-owner guard (task #36 / plan §5.4): refuse if THIS op is
     * already running (s_fwbusy) OR any other fast-op owns the bus (fast-read sets
     * FLASH_ACTIVE_BIT too), so fast-read and fast-write mutually exclude. */
    if (s_fwbusy || can_flash_active())
    {
        ESP_LOGW(TAG, "fast write refused: another fast-op in progress");
        return -1;
    }
    s_fwbusy = 1;
    /* Claim the bus BEFORE suspending can_rx_task (plan §5.2): the datalogger poll
     * task parks on this bit, so no stray 0x7E0 can corrupt the UDS session. */
    can_flash_active_set();

    /* Variables the cleanup label touches MUST be declared before any goto. */
    FILE *f = NULL;
    int was_suspended = 0;
    int host_gone = 0;
    int rc = 0;
    TaskHandle_t rx_task = NULL;
    fw_manifest_t m;
    /* Event-log milestone bookkeeping (Task #12): total_blocks/done are read by the cleanup label
     * (EVL_FLASH_FAIL), so they must be declared before any goto. fw_t0_us is captured for real at
     * FLASH_START (the about-to-touch-ECU point), not here. */
    int64_t fw_t0_us = 0;
    uint32_t total_blocks = 0;
    uint32_t done = 0;            /* blocks completed; on FAIL this is "where it died" */
    s_fw_err_stage = 0;
    s_fw_err_nrc = 0;
    /* Per-op keepalive/PONR state. Cleared HERE, not at cleanup, so a previous op's counters can
     * still be read by its own cleanup, and so a crash mid-op cannot leave s_ponr latched into
     * the next one. */
    s_ponr = 0;
    memset(&s_ka, 0, sizeof(s_ka));

    char img_path[160];
    char man_path[160];
    char stem[97];
    strlcpy(stem, name, sizeof(stem));
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    snprintf(img_path, sizeof(img_path), "%s/%s", FW_ROMS_DIR, name);
    snprintf(man_path, sizeof(man_path), "%s/%s.json", FW_ROMS_DIR, stem);

    int live = (mode == 'L');
    const char *mode_str = live ? "LIVE" : "dry-run";

    ESP_LOGI(TAG, "fast write %s: %s", mode_str, img_path);

    /* Take exclusive CAN ownership (mirrors live; also exercises the teardown). */
    rx_task = xTaskGetHandle("can_rx_task");
    if (rx_task) { vTaskSuspend(rx_task); was_suspended = 1; }
    twai_message_t junk;
    while (can_receive(&junk, 0) == ESP_OK) { /* drain */ }
    xdev_buffer txjunk;
    while (xQueueReceive(*tx_queue, &txjunk, 0) == pdTRUE) { /* discard */ }

    if (!sdcard_is_mounted()) { fw_emit_err(tx_queue, 0, 1, 0); rc = -2; goto cleanup; }

    /* 1. Manifest. */
    if (fw_load_manifest(man_path, &m) != 0) { fw_emit_err(tx_queue, 0, 2, 0); rc = -2; goto cleanup; }
    if (m.manifest_version != 1 || m.block_size == 0 || m.block_size > 1024 ||
        m.sbl_offset + m.sbl_len > m.image_len ||
        m.program_offset + m.program_len > m.image_len)
    {
        fw_emit_err(tx_queue, 0, 3, 0);
        rc = -2;
        goto cleanup;
    }

    /* 2. Open staged image, check declared size. */
    f = fopen(img_path, "rb");
    if (!f) { fw_emit_err(tx_queue, 0, 1, 0); rc = -2; goto cleanup; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz != (long)m.image_len) { fw_emit_err(tx_queue, 0, 4, 0); rc = -2; goto cleanup; }

    /* 3. PRE-ERASE INTEGRITY GATE (hard block): CRC32 over the whole staged image
     *    must equal the manifest. A corrupt upload aborts here with NO ECU contact. */
    {
        uint32_t crc = 0xFFFFFFFFu;
        size_t got;
        while ((got = fread(s_fw_chunk, 1, sizeof(s_fw_chunk), f)) > 0)
            crc = fw_crc32_step(crc, s_fw_chunk, got);
        crc ^= 0xFFFFFFFFu;
        if (crc != m.image_crc32)
        {
            ESP_LOGE(TAG, "digest gate FAIL: crc 0x%08lX != manifest 0x%08lX",
                     (unsigned long)crc, (unsigned long)m.image_crc32);
            fw_emit_err(tx_queue, 0, 5, 0);
            rc = -2;
            goto cleanup;
        }
        ESP_LOGI(TAG, "digest gate OK: crc 0x%08lX", (unsigned long)crc);
    }

    /* HARD SAFETY GATE: a live flash is only permitted in an NCFW_ALLOW_LIVE build. */
    if (live && !NCFW_ALLOW_LIVE)
    {
        ESP_LOGW(TAG, "live flash refused: this is a DRY-RUN build (NCFW_ALLOW_LIVE=0)");
        fw_emit_err(tx_queue, 0, 7, 0);
        rc = -2;
        goto cleanup;
    }

    uint32_t bs = m.block_size;
    uint32_t sbl_blocks = (m.sbl_len + bs - 1) / bs;
    uint32_t prog_blocks = (m.program_len + bs - 1) / bs;
    total_blocks = sbl_blocks + prog_blocks;

    /* Operational milestone (Task #12): the flash is past every pre-erase gate and is about to
     * touch the ECU. One sparse line -- the per-block progress stays on the wire (NCFWPROG). */
    fw_t0_us = esp_timer_get_time();
    event_log_emit(EVL_FLASH_START, "%.48s %s blocks=%lu",
                   name, mode_str, (unsigned long)total_blocks);

    if (fw_emit(tx_queue, "NCFWSYNC\n") != 0) { host_gone = 1; rc = -3; goto cleanup; }

    if (live)
    {
        int nrc = 0;
        if (fw_request_download(m.download_addr, m.download_size, &nrc) != 0)
        {
            fw_emit_err(tx_queue, m.download_addr, 10, nrc);
            rc = -2;
            goto cleanup;
        }
    }

    /* 4/5. Walk SBL region then program region: read each block from the staged
     *      image and (live only) TransferData it. Stream NCFWPROG periodically. */
    uint32_t regions[2][2] = {
        {m.sbl_offset, m.sbl_len},
        {m.program_offset, m.program_len},
    };
    for (int r = 0; r < 2; r++)
    {
        uint32_t off = regions[r][0];
        uint32_t rem = regions[r][1];
        while (rem > 0)
        {
            uint32_t take = (rem > bs) ? bs : rem;
            if (fseek(f, (long)off, SEEK_SET) != 0 ||
                fread(&s_fw_msg[1], 1, take, f) != take)
            {
                fw_emit_err(tx_queue, off, 6, 0);
                rc = -2;
                goto cleanup;
            }

            if (live)
            {
                int nrc = 0;
                /* The erase edge (#126). The ECU jumps into the SBL once the SBL region is fully
                 * transferred, and the SBL erases before it answers, so the stall lands on the ACK
                 * of the LAST SBL block or on the FIRST program block depending on how the ECU
                 * sequences it. Cover both -- guessing wrong costs an abandoned mid-erase ECU. */
                const bool erase_edge = (r == 0 && rem == take) ||
                                        (r == 1 && off == regions[1][0]);
                if (erase_edge)
                {
                    /* Reset the host's idle clock at the START of this stall.
                     *
                     * Both edge blocks arm their OWN RESP_ERASE_TIMEOUT_MS, so an ECU that splits
                     * its erase across the two can lawfully keep us silent for 60 + 60 s. The
                     * host gives up after _FAST_WRITE_IDLE_MS (90 s) of no bytes, and when it does
                     * it closes the socket -- slcan_port_tx_task then parks on PORT_OPEN and stops
                     * draining, our next NCFWPROG fills the queue, tx_send times out, and the
                     * flash aborts via host_gone with the ECU freshly erased. Exactly the outcome
                     * this whole change exists to prevent.
                     *
                     * One line here bounds EVERY silent window to one edge budget (~62 s) instead
                     * of two, whichever way the ECU splits the erase, and each edge keeps its full
                     * 60 s. Off-cadence NCFWPROG lines are harmless to the host parser -- worst
                     * case the progress callback repeats a count -- and any bytes reset its clock.
                     *
                     * Raising the host timeout instead would only make a genuinely dead firmware
                     * take longer to notice. If this emit fails the host is already gone, and
                     * aborting HERE is pre-erase for the first edge: strictly safer than today. */
                    char eline[40];
                    snprintf(eline, sizeof(eline), "NCFWPROG %lu/%lu\n",
                             (unsigned long)done, (unsigned long)total_blocks);
                    if (fw_emit(tx_queue, eline) != 0) { host_gone = 1; rc = -3; goto cleanup; }
                }

                /* POINT OF NO RETURN. Set before the LAST SBL block leaves, because the ECU may
                 * start erasing on that block's ACK or on the next one (see the comment above),
                 * and taking it any later leaves a window where an abort abandons an erasing ECU.
                 * The pre-edge emit above still runs pre-PONR for this first edge, where aborting
                 * is genuinely free; at the region-1 edge s_ponr is already set, so that same
                 * emit can no longer cost us the ECU.
                 *
                 * Keyed on erase_edge, NOT on (r == 0 && rem == take): a manifest declaring
                 * sbl_len = 0 skips region 0 entirely -- the block-size gate allows it -- so the
                 * narrower test would never fire, yet region 1's first block still arms the
                 * keepalive below. Every post-edge emit would then be a blocking send whose
                 * failure aborts, which is precisely the brick this change removes. This form
                 * also makes "armed implies past the point of no return" true at all three arm
                 * sites. (No assert on that invariant: a panic mid-flash is itself a brick.) */
                if (erase_edge) s_ponr = 1;

                /* Taken AFTER the emit above, so the budget covers only the ECU's stall. */
                const int64_t edge_t0 = esp_timer_get_time();
                const int64_t edge_deadline =
                    erase_edge ? edge_t0 + (int64_t)RESP_ERASE_TIMEOUT_MS * 1000 : 0;
                /* Feed the host through the silence so its idle clock never fires (#126
                 * follow-up). Armed only at the edge: ordinary blocks answer in milliseconds. */
                if (erase_edge) fw_ka_arm(tx_queue, done, total_blocks);
                int tdrc = fw_transfer_data(take, &nrc, edge_deadline);
                fw_ka_disarm();
                if (tdrc != 0)
                {
                    fw_emit_err(tx_queue, off, r == 0 ? 11 : 12, nrc);
                    rc = -2;
                    goto cleanup;
                }
                if (erase_edge)
                {
                    /* How long this ECU really takes to erase -- the number that sizes
                     * RESP_ERASE_TIMEOUT_MS instead of guessing at it. Measured 2026-08-23 on a
                     * live PCM: 12.6 s for a full ROM, 1.4 s for a 134-block image, and 46 ms at
                     * the region-1 edge (this ECU erases once, at the region-0 edge, and the
                     * region-1 wait is a non-event).
                     *
                     * Only waits of a second or more are logged. Below that nothing stalled and
                     * the line is noise -- but the threshold is the ONLY filter, so an ECU that
                     * ever did erase at region 1 would still show up here. Message is in seconds
                     * and carries no region label, by owner request: it is read by the person
                     * watching a progress bar sit still, not by the flash code. */
                    const uint32_t edge_ms =
                        (uint32_t)((esp_timer_get_time() - edge_t0) / 1000);
                    if (edge_ms >= 1000)
                    {
                        const uint32_t r10 = edge_ms + 50; /* round to a tenth, not truncate */
                        event_log_emit(EVL_INFO,
                                       "waited %lu.%lu s while the ECU cleared its memory for the new ROM",
                                       (unsigned long)(r10 / 1000),
                                       (unsigned long)((r10 % 1000) / 100));
                    }
                }
            }

            off += take;
            rem -= take;
            done++;
            s_ka.done = done;   /* keeps fw_ka_note_drop()'s "where did we lose them" honest */
            if ((done % PROG_EVERY_N) == 0 || done == total_blocks)
            {
                char line[40];
                snprintf(line, sizeof(line), "NCFWPROG %lu/%lu\n",
                         (unsigned long)done, (unsigned long)total_blocks);
                if (fw_emit(tx_queue, line) != 0) { host_gone = 1; rc = -3; goto cleanup; }
            }
            if ((done & 0x1F) == 0) vTaskDelay(1); /* WDT yield */
        }
    }

    if (live)
    {
        int nrc = 0;
        /* Third arm site: TransferExit carries the same 60 s ceiling, and the ECU may verify or
         * finalise there in silence. Without a beat, a finalise past 30 s makes an NC Flash
         * 2.12.0 host report failure on a flash that actually succeeded, sending the user into a
         * needless second flash of a healthy ECU. */
        fw_ka_arm(tx_queue, total_blocks, total_blocks);
        int terc = fw_transfer_exit(&nrc);
        fw_ka_disarm();
        if (terc != 0)
        {
            fw_emit_err(tx_queue, 0, 13, nrc);
            rc = -2;
            goto cleanup;
        }
        fw_ecu_reset(); /* best-effort */
    }

    (void)fw_emit(tx_queue, "NCFWDONE\n");
    ESP_LOGI(TAG, "fast write %s complete: %lu blocks",
             mode_str, (unsigned long)total_blocks);
    event_log_emit(EVL_FLASH_OK, "%.48s %s blocks=%lu elapsed=%lldms",
                   name, mode_str, (unsigned long)total_blocks,
                   (long long)((esp_timer_get_time() - fw_t0_us) / 1000));
    rc = 0;

cleanup:
    if (f) fclose(f);
    if (was_suspended && rx_task) vTaskResume(rx_task);
    {
        twai_message_t drain;
        while (can_receive(&drain, 0) == ESP_OK) { /* discard stale RX */ }
    }
    {
        uint32_t alerts = 0;
        (void)twai_read_alerts(&alerts, 0);
    }
    /* Drain only when nobody can still be waiting for a good line.
     *
     * NOT on dropped alone: a dropped line does not mean the host is dead NOW. A host that
     * stalled and recovered, or a client that reconnected mid-flash, is alive and waiting for
     * NCFWDONE -- and DONE is queued just above, so draining here would swallow it and report
     * failure for a flash that worked, pushing the user into a needless re-flash of a healthy
     * ECU. host_gone is provably pre-PONR now (past it, no emit failure sets it), so on that
     * path DONE was never queued and there is nothing to lose.
     *
     * Not on dropped at all, even when the flash failed: a host that stalled and recovered is
     * still waiting for the FWERR naming WHY it failed, and draining would replace that with a
     * bare idle timeout. Stale lines left for a genuinely dead host cost nothing -- the next op
     * drains the queue before it starts, and version_ping ignores lines it does not know. */
    if (host_gone)
    {
        xdev_buffer leftover;
        while (xQueueReceive(*tx_queue, &leftover, 0) == pdTRUE) { /* discard */ }
        if (host_gone)
            ESP_LOGW(TAG, "fast write aborted: host stopped draining TCP (clean teardown)");
    }
    /* The host stopped taking our lines while we were past the point of no return. Without this
     * line the flash just looks successful and nobody can explain why the PC tool reported a
     * stall. Word it by outcome: on rc == 0 we really did finish the ECU alone; on a failure the
     * ECU is why we stopped, and claiming we "carried on" would be a lie. Report the block where
     * the FIRST line went undelivered -- `done` at cleanup equals total_blocks on success and
     * would always read N/N. */
    if (s_ka.dropped)
    {
        ESP_LOGW(TAG, "fast write: %lu progress lines undelivered (host stopped listening)",
                 (unsigned long)s_ka.dropped);
        if (rc == 0)
            event_log_emit(EVL_INFO,
                           "PC tool stopped listening at block %lu/%lu -- the flash finished without it",
                           (unsigned long)s_ka.drop_blk, (unsigned long)total_blocks);
        else
            event_log_emit(EVL_INFO,
                           "PC tool stopped listening at block %lu/%lu before the flash failed",
                           (unsigned long)s_ka.drop_blk, (unsigned long)total_blocks);
    }
    /* Operational milestone (Task #12): any non-zero rc is an aborted/failed flash. One sparse
     * line naming WHERE it died -- the fw_emit_err stage + the FWSUB_* or NRC sub-code + the block
     * index reached (done/total) -- so a post-mortem of a wireless flash is a one-line lookup.
     * Stash is set by fw_emit_err; host-gone aborts (rc=-3) carry no stage, so flag them. The
     * load-bearing fixed fields come FIRST and the variable-length ROM name LAST (capped), so a
     * long filename can only ever truncate itself in the 112-byte detail, never the diagnostics. */
    if (rc != 0)
    {
        event_log_emit(EVL_FLASH_FAIL, "%s rc=%d st=%d nrc=0x%02X blk=%lu/%lu%s name=%.48s",
                       mode_str, rc, s_fw_err_stage,
                       s_fw_err_nrc & 0xFF, (unsigned long)done, (unsigned long)total_blocks,
                       host_gone ? " host_gone" : "", name);
    }
    /* Release the bus LAST (task #36 / plan §5.2): only now -- after can_rx_task is
     * resumed and the bus drained -- does the poll task un-park, so the single-CAN-
     * owner invariant holds on EVERY exit path (success / host-gone / abort). */
    /* Drop the point-of-no-return here as well as at op start. It is latched for the whole
     * post-erase phase, and leaving it set past cleanup would make the NEXT op's early errors --
     * the unsafe-name FWERR, which runs before the op-start reset -- go out as droppable 0-tick
     * sends and vanish silently. */
    s_ponr = 0;
    can_flash_active_clear();
    s_fwbusy = 0;
    return rc;
}
