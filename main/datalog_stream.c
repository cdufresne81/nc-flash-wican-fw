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

/*
 * Always-on live-datalog TCP stream listener (issue #3). See datalog_stream.h for the wire
 * protocol and rationale. This is a deliberate, tail-only clone of slcan_port.c's TCP path
 * (server/rx/tx tasks, event-group OPEN/CLOSED handshake, keepalive, single client) so it
 * shares NO module state with the hardware-proven flash/coexistence path -- nothing here can
 * perturb the datalogger. It touches no CAN bit, no park/claim/lease, and adds no config key.
 *
 * Data path: the csv writer task fires datalog_stream_csv_hook() -> whole lines are pushed
 * into an INTERNAL-RAM StreamBuffer (single writer) with 0-timeout, drop-newest backpressure;
 * the tx task is the single reader and drains it to the socket.
 *
 * Announce protocol (the part that must be exactly-once): session boundaries (#close, and
 * #session + the whole header line) are ANNOUNCED, not best-effort. s_session.seq is bumped
 * under s_hdr_mutex on every state change; the tx task's connect handshake loops until the
 * state it sent is provably current and only then sets PORT_CONN under the same mutex, while
 * the hook reads PORT_CONN under that mutex before announcing -- so for every state change
 * exactly one side announces it. Under backpressure a boundary announce is retried (sync
 * block, all-or-nothing, always ahead of any row); only rows are ever dropped, and a "#drop n"
 * marker precedes the next successfully streamed line.
 */

#include <string.h>
#include <errno.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "datalog_stream.h"

#define TAG __func__

#define KEEPALIVE_IDLE      5
#define KEEPALIVE_INTERVAL  5
#define KEEPALIVE_COUNT     3
/* Bound on a stalled send(): a live-but-not-reading client (zero TCP window) must not wedge
 * the tx task inside send() holding the socket mutex forever -- keepalive only detects DEAD
 * peers. On timeout the connection is dropped and the client can reconnect. */
#define SEND_TIMEOUT_S      10

#define PORT_CLOSED_BIT     BIT0   /* server waits on this to close+re-accept                 */
#define PORT_OPEN_BIT       BIT1   /* a client socket has been accepted (wakes rx/tx)          */
#define PORT_CONN_BIT       BIT2   /* hook enqueue enabled: set by tx AFTER the state handshake */

#define STREAM_BUF_SIZE     (16 * 1024)   /* writer->tx ring, INTERNAL RAM (brick-safe)       */
#define HDR_COPY_MAX        4096          /* header-line copy; larger headers -> #nohdr        */
#define DRAIN_BUF_SIZE      1024          /* tx ring-drain scratch                             */
/* Bounded (never portMAX_DELAY: the hook runs on the csv writer task) wait for the header
 * mutex. The only other holder is the tx task, which holds it for a memcpy / bit-set, so this
 * never actually elapses; if it ever did, the header copy is flagged unreliable (#nohdr). */
#define HDR_MUTEX_WAIT      pdMS_TO_TICKS(100)

static uint32_t s_port = 0;
static int s_sock = -1;
static int s_listen_sock = -1;
/* Bumped by the server task on every accept() (under s_sock_mutex, together with the s_sock
 * swap). rx/tx snapshot it and discard results/teardowns whose generation moved on -- otherwise
 * a stale recv/send on a just-closed fd could clear the FRESH PORT bits of (or write old bytes
 * into) a new connection on a fast host reconnect. Purely internal (never wired to a lease or
 * the reaper); the stream is not a coexistence owner. */
static volatile uint32_t s_conn_gen = 0;

static EventGroupHandle_t s_event_group = NULL;
static StaticEventGroup_t s_event_group_buffer;
static SemaphoreHandle_t s_sock_mutex = NULL;   /* serializes send()/close()/fd swap           */

/* Writer->tx StreamBuffer, storage in INTERNAL RAM (the csv writer task enqueues from inside
 * flash-cache-disable windows, where any PSRAM access faults -- brick invariant). */
static StreamBufferHandle_t s_stream = NULL;
static StaticStreamBuffer_t s_stream_struct;
static uint8_t *s_stream_storage = NULL;

/* Session/header state shared between the hook (writer) and the tx task (join/handshake
 * reader). Guarded by s_hdr_mutex, held ONLY across memcpy/flag updates and the PORT_CONN
 * bit-set (all microsecond-bounded; never across a send()). All in .bss => INTERNAL RAM
 * (the hook is on the writer task). */
static SemaphoreHandle_t s_hdr_mutex = NULL;
static char   s_hdr[HDR_COPY_MAX];
static size_t s_hdr_len = 0;
static bool   s_hdr_complete = false;
static volatile bool s_hdr_overflow = false;   /* copy too big / could not be updated -> #nohdr */
static struct {
    bool active;
    int  cols;
    char file[64];
    uint32_t seq;    /* bumped on SESSION_OPEN / HDR_END / CLOSE: the announce version */
} s_session;

/* Hook-side announce/backpressure state. Single writer = the hook (csv writer task), except
 * the tx task's connect-handshake reset, which happens under s_hdr_mutex while PORT_CONN is
 * still clear (the hook only touches these when PORT_CONN is set). */
static bool s_resync_needed = false;           /* a session boundary still has to go on the wire */
static uint32_t s_pending_drops = 0;           /* whole lines dropped since the last good line   */
static volatile uint32_t s_rows_sent = 0;
static volatile uint32_t s_rows_dropped = 0;

/* tx-task-only header snapshot (not touched by the hook). Static so it does not sit on the
 * 4 KB PSRAM task stack. */
static char s_tx_snapshot[HDR_COPY_MAX];

#ifndef GIT_SHA
#define GIT_SHA "unknown"
#endif

static const char *datalog_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

static int format_session_line(char *out, size_t cap)
{
    int n = snprintf(out, cap, "#session file=%s cols=%d\n", s_session.file, s_session.cols);
    if (n <= 0) return -1;
    return MIN(n, (int)cap - 1);
}

/* Non-blocking, all-or-nothing enqueue of one or two line-aligned parts, prepending a
 * "#drop <n>" marker when losses precede them. Returns false (with NO accounting -- callers
 * decide whether a failure means "lost" or "retry later") when the ring lacks room for the
 * whole thing. Hook context only (single StreamBuffer writer); the tx task only ever frees
 * ring space, so the SpacesAvailable check guarantees complete, never-torn writes. */
static bool stream_try_enqueue(const char *a, size_t alen, const char *b, size_t blen)
{
    if (s_stream == NULL || a == NULL || alen == 0)
    {
        return false;
    }
    char marker[24];
    size_t mlen = 0;
    if (s_pending_drops != 0)
    {
        int m = snprintf(marker, sizeof(marker), "#drop %u\n", (unsigned)s_pending_drops);
        if (m > 0) mlen = MIN((size_t)m, sizeof(marker) - 1);
    }
    if (xStreamBufferSpacesAvailable(s_stream) < mlen + alen + blen)
    {
        return false;
    }
    if (mlen) xStreamBufferSend(s_stream, marker, mlen, 0);
    xStreamBufferSend(s_stream, a, alen, 0);
    if (b != NULL && blen) xStreamBufferSend(s_stream, b, blen, 0);
    s_pending_drops = 0;
    return true;
}

/* Put the pending session boundary on the wire: "#close" when idle, else "#session" plus the
 * WHOLE header line (or #nohdr) as one all-or-nothing block, so the receiver can never adopt
 * a data row as the header or miss a rotation. Retried on every subsequent hook event until
 * it fits; rows are dropped (never reordered ahead of it) while it is pending. Hook context
 * only -- the hook is the single writer of s_session/s_hdr, so no mutex is needed to read. */
static void stream_sync_flush(void)
{
    if (!s_resync_needed)
    {
        return;
    }
    bool ok;
    if (!s_session.active)
    {
        ok = stream_try_enqueue("#close\n", 7, NULL, 0);
    }
    else
    {
        char line[96];
        int n = format_session_line(line, sizeof(line));
        if (n <= 0)
        {
            return;
        }
        if (s_hdr_complete && !s_hdr_overflow && s_hdr_len > 0)
        {
            ok = stream_try_enqueue(line, (size_t)n, s_hdr, s_hdr_len);
        }
        else
        {
            ok = stream_try_enqueue(line, (size_t)n, "#nohdr\n", 7);
        }
    }
    if (ok)
    {
        s_resync_needed = false;
    }
}

void datalog_stream_csv_hook(csv_stream_event_t ev, const char *data, size_t len)
{
    /* s_event_group is published LAST in datalog_stream_init(), so non-NULL here means every
     * other handle (mutexes, ring) is valid too. */
    if (s_event_group == NULL)
    {
        return;
    }

    switch (ev)
    {
    case CSV_STREAM_EV_SESSION_OPEN:
    {
        /* Reset the header accumulator and record the new session (basename + column count;
         * data = NUL-terminated file path, len = column count -- csv_logger.h). The announce
         * itself is deferred to HDR_END: the sync block needs the complete header line, and
         * no row can arrive before HDR_END. */
        const char *base = (data != NULL) ? datalog_basename(data) : "";
        if (xSemaphoreTake(s_hdr_mutex, HDR_MUTEX_WAIT) == pdTRUE)
        {
            s_hdr_len = 0;
            s_hdr_complete = false;
            s_hdr_overflow = false;
            s_session.active = true;
            s_session.cols = (int)len;
            strlcpy(s_session.file, base, sizeof(s_session.file));
            s_session.seq++;
            xSemaphoreGive(s_hdr_mutex);
        }
        else
        {
            s_hdr_overflow = true;   /* could not reset the copy -> receivers get #nohdr */
        }
        break;
    }

    case CSV_STREAM_EV_HDR_CHUNK:
        /* Accumulate only -- the header goes on the wire as ONE whole line at HDR_END, never
         * chunk-by-chunk, so backpressure can drop it whole (and re-announce) but never tear it. */
        if (data == NULL || len == 0)
        {
            break;
        }
        if (xSemaphoreTake(s_hdr_mutex, HDR_MUTEX_WAIT) == pdTRUE)
        {
            if (!s_hdr_overflow && s_hdr_len + len <= sizeof(s_hdr))
            {
                memcpy(s_hdr + s_hdr_len, data, len);
                s_hdr_len += len;
            }
            else
            {
                s_hdr_overflow = true;   /* header line > HDR_COPY_MAX -> receivers get #nohdr */
            }
            xSemaphoreGive(s_hdr_mutex);
        }
        else
        {
            s_hdr_overflow = true;
        }
        break;

    case CSV_STREAM_EV_HDR_END:
    {
        /* The session (with its header) is now announceable. Reading PORT_CONN under the same
         * mutex the tx handshake uses to set it makes the announce exactly-once: either we see
         * PORT_CONN and announce via the ring, or the tx task sees the bumped seq and sends the
         * fresh state itself before enabling the live path. */
        bool connected = false;
        if (xSemaphoreTake(s_hdr_mutex, HDR_MUTEX_WAIT) == pdTRUE)
        {
            s_hdr_complete = !s_hdr_overflow;
            s_session.seq++;
            connected = (xEventGroupGetBits(s_event_group) & PORT_CONN_BIT) != 0;
            xSemaphoreGive(s_hdr_mutex);
        }
        if (connected)
        {
            s_resync_needed = true;
            stream_sync_flush();
        }
        break;
    }

    case CSV_STREAM_EV_ROW:
    {
        bool connected = (xEventGroupGetBits(s_event_group) & PORT_CONN_BIT) != 0;
        if (!connected || data == NULL || len == 0)
        {
            break;
        }
        /* Never let a row overtake a pending session boundary. */
        if (s_resync_needed)
        {
            stream_sync_flush();
            if (s_resync_needed)
            {
                s_pending_drops++;
                s_rows_dropped++;
                break;
            }
        }
        if (stream_try_enqueue(data, len, NULL, 0))
        {
            s_rows_sent++;
        }
        else
        {
            s_pending_drops++;
            s_rows_dropped++;
        }
        break;
    }

    case CSV_STREAM_EV_CLOSE:
    {
        bool connected = false;
        if (xSemaphoreTake(s_hdr_mutex, HDR_MUTEX_WAIT) == pdTRUE)
        {
            s_hdr_complete = false;      /* no header until the next session */
            s_session.active = false;
            s_session.seq++;
            connected = (xEventGroupGetBits(s_event_group) & PORT_CONN_BIT) != 0;
            xSemaphoreGive(s_hdr_mutex);
        }
        if (connected)
        {
            s_resync_needed = true;
            stream_sync_flush();
        }
        break;
    }

    default:
        break;
    }
}

/* Send a whole buffer with a partial-write loop, guarded by the socket mutex (mirrors
 * slcan_port_tx). `gen` is the tx task's connection-generation snapshot: if the server has
 * meanwhile closed this connection and accepted a NEW one (fast host reconnect while a send
 * was stalled), s_sock already belongs to the new client -- bail out instead of writing the
 * old connection's bytes into the new stream. The gen check is under the same mutex as the
 * server's fd swap, so it cannot race it. Returns 0 on success, -1 to drop. */
static int stream_send_all(uint32_t gen, const uint8_t *buf, int len)
{
    if (xSemaphoreTake(s_sock_mutex, portMAX_DELAY) != pdTRUE)
    {
        return -1;
    }
    int to_write = len;
    while (to_write > 0)
    {
        if (gen != s_conn_gen)
        {
            xSemaphoreGive(s_sock_mutex);
            return -1;
        }
        int written = send(s_sock, buf + (len - to_write), to_write, 0);
        if (written < 0)
        {
            ESP_LOGE(TAG, "Error during sending: errno %d", errno);
            xSemaphoreGive(s_sock_mutex);
            return -1;
        }
        to_write -= written;
    }
    xSemaphoreGive(s_sock_mutex);
    return 0;
}

/* Blocking recv purely for disconnect detection; received bytes are discarded (the host never
 * sends on this stream). Mirrors slcan_port_rx_task including the stale-recv generation guard. */
static void datalog_stream_rx_task(void *pvParameters)
{
    static uint8_t rx_discard[256];

wait_skt_rx:
    xEventGroupWaitBits(s_event_group, PORT_OPEN_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    while (1)
    {
        uint32_t gen = s_conn_gen;   /* snapshot before blocking in recv */
        int r = recv(s_sock, rx_discard, sizeof(rx_discard), 0);
        if (xSemaphoreTake(s_sock_mutex, portMAX_DELAY) == pdTRUE)
        {
            if (gen != s_conn_gen)
            {
                /* connection changed while blocked -> result is for a dead fd, drop it without
                 * touching the PORT bits (a new connection may already have armed PORT_OPEN). */
                xSemaphoreGive(s_sock_mutex);
                goto wait_skt_rx;
            }
            if (r < 0)
            {
                xEventGroupSetBits(s_event_group, PORT_CLOSED_BIT);
                xEventGroupClearBits(s_event_group, PORT_OPEN_BIT | PORT_CONN_BIT);
                ESP_LOGE(TAG, "Error during receiving: errno %d", errno);
                xSemaphoreGive(s_sock_mutex);
                goto wait_skt_rx;
            }
            else if (r == 0)
            {
                xEventGroupSetBits(s_event_group, PORT_CLOSED_BIT);
                xEventGroupClearBits(s_event_group, PORT_OPEN_BIT | PORT_CONN_BIT);
                ESP_LOGW(TAG, "Connection closed");
                xSemaphoreGive(s_sock_mutex);
                goto wait_skt_rx;
            }
            /* else: unexpected inbound bytes on a tail-only stream -> discard and keep reading. */
            xSemaphoreGive(s_sock_mutex);
        }
    }
}

/* Drain the writer->tx StreamBuffer to the connected client. On accept it sends #hello, then
 * runs the state handshake: send the current session state, and under s_hdr_mutex either
 * observe it unchanged (enable the hook's live path atomically -- PORT_CONN) or loop with the
 * fresh state. This closes the connect-races-session-open gap: a session opening or closing
 * during the preamble is either resent by us or announced by the hook, never lost. */
static void datalog_stream_tx_task(void *pvParameters)
{
    static uint8_t drain_buf[DRAIN_BUF_SIZE];

wait_skt_tx:
    xEventGroupWaitBits(s_event_group, PORT_OPEN_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "Datalog stream client connected");
    /* Generation of the connection this pass serves; every send checks it so a stalled tx can
     * never write into (or tear down) a NEWER connection after a fast host reconnect. */
    uint32_t gen = s_conn_gen;

    /* Drain-discard anything left from a previous client before the live path opens.
     * Deliberately NOT xStreamBufferReset(): reset is unsafe against a concurrent hook
     * enqueue, whereas concurrent send/receive is exactly the buffer's supported
     * single-writer/single-reader mode. */
    while (xStreamBufferReceive(s_stream, drain_buf, sizeof(drain_buf), 0) > 0) {}

    {
        char line[96];
        int n = snprintf(line, sizeof(line), "#hello NCDLv1 fw=%s\n", GIT_SHA);
        if (n > 0)
        {
            n = MIN(n, (int)sizeof(line) - 1);
            if (stream_send_all(gen, (const uint8_t *)line, n) != 0) goto drop_conn;
        }
    }

    /* State handshake loop (see the file-top "Announce protocol" note). */
    for (;;)
    {
        bool sess_active;
        char sess_line[96];
        int  sess_n = 0;
        bool hdr_ok = false;
        size_t hdr_len = 0;
        uint32_t seq;

        if (xSemaphoreTake(s_hdr_mutex, portMAX_DELAY) != pdTRUE) goto drop_conn;
        sess_active = s_session.active;
        seq = s_session.seq;
        if (sess_active)
        {
            sess_n = format_session_line(sess_line, sizeof(sess_line));
            hdr_ok = s_hdr_complete && !s_hdr_overflow &&
                     s_hdr_len > 0 && s_hdr_len <= sizeof(s_tx_snapshot);
            if (hdr_ok)
            {
                hdr_len = s_hdr_len;
                memcpy(s_tx_snapshot, s_hdr, hdr_len);
            }
        }
        xSemaphoreGive(s_hdr_mutex);

        if (sess_active)
        {
            if (sess_n > 0 && stream_send_all(gen, (const uint8_t *)sess_line, sess_n) != 0) goto drop_conn;
            if (hdr_ok)
            {
                if (stream_send_all(gen, (const uint8_t *)s_tx_snapshot, (int)hdr_len) != 0) goto drop_conn;
            }
            else
            {
                if (stream_send_all(gen, (const uint8_t *)"#nohdr\n", 7) != 0) goto drop_conn;
            }
        }
        else
        {
            if (stream_send_all(gen, (const uint8_t *)"#idle\n", 6) != 0) goto drop_conn;
        }

        if (xSemaphoreTake(s_hdr_mutex, portMAX_DELAY) != pdTRUE) goto drop_conn;
        if (seq == s_session.seq)
        {
            /* The state we sent is current. Arm the live path atomically with that knowledge;
             * also reset the hook-side backpressure state so this client never inherits a
             * phantom "#drop" or a stale resync from a previous connection. (PORT_CONN is
             * still clear here, so the hook is not touching these.) */
            s_resync_needed = false;
            s_pending_drops = 0;
            xEventGroupSetBits(s_event_group, PORT_CONN_BIT);
            xSemaphoreGive(s_hdr_mutex);
            break;
        }
        xSemaphoreGive(s_hdr_mutex);
        /* Session state changed while we were sending -- go around with a fresh snapshot. */
    }

    while (1)
    {
        size_t got = xStreamBufferReceive(s_stream, drain_buf, sizeof(drain_buf), pdMS_TO_TICKS(250));
        if (got == 0)
        {
            /* idle tick: bail if the connection died or was replaced (fast reconnect) */
            if (!(xEventGroupGetBits(s_event_group) & PORT_OPEN_BIT) || gen != s_conn_gen)
            {
                goto wait_skt_tx;
            }
            continue;
        }
        if (stream_send_all(gen, drain_buf, (int)got) != 0) goto drop_conn;
    }

drop_conn:
    /* Tear down only OUR connection: if the generation moved on, a newer connection already
     * owns the port bits (same stale-guard idea as the rx task). */
    if (xSemaphoreTake(s_sock_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (gen == s_conn_gen)
        {
            xEventGroupSetBits(s_event_group, PORT_CLOSED_BIT);
            xEventGroupClearBits(s_event_group, PORT_OPEN_BIT | PORT_CONN_BIT);
        }
        xSemaphoreGive(s_sock_mutex);
    }
    goto wait_skt_tx;
}

/* Bind/listen/accept loop for the stream port (mirrors slcan_port_server_task: 1 s bind-retry
 * forever, single client, keepalive, event-group OPEN/CLOSED handshake). */
static void datalog_stream_server_task(void *pvParameters)
{
    char addr_str[128];
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct timeval snd_timeout = { .tv_sec = SEND_TIMEOUT_S, .tv_usec = 0 };
    struct sockaddr_storage dest_addr;
    struct sockaddr_storage source_addr;
    socklen_t addr_len = sizeof(source_addr);

    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(s_port);

    /* Retry the listen socket forever so a transient netif-not-ready at boot self-heals instead
     * of permanently killing the listener; each failed attempt frees its fd before retrying. */
    for (;;)
    {
        s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (s_listen_sock < 0)
        {
            ESP_LOGE(TAG, "Unable to create socket: errno %d (retry)", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int opt = 1;
        setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(s_listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0)
        {
            ESP_LOGE(TAG, "Socket unable to bind port %lu: errno %d (retry)", s_port, errno);
            close(s_listen_sock);
            s_listen_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (listen(s_listen_sock, 1) != 0)
        {
            ESP_LOGE(TAG, "Error during listen: errno %d (retry)", errno);
            close(s_listen_sock);
            s_listen_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        break;
    }
    ESP_LOGI(TAG, "Datalog stream bound + listening, port %lu", s_port);

    while (1)
    {
        ESP_LOGI(TAG, "Datalog stream listening");
    accept_socket:
        {
            int new_sock = accept(s_listen_sock, (struct sockaddr *)&source_addr, &addr_len);
            if (new_sock < 0)
            {
                ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
                goto accept_socket;
            }
            setsockopt(new_sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
            setsockopt(new_sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
            setsockopt(new_sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
            setsockopt(new_sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
            setsockopt(new_sock, SOL_SOCKET, SO_SNDTIMEO, &snd_timeout, sizeof(snd_timeout));
            /* Swap the live fd and bump the generation atomically w.r.t. every send()/teardown
             * (all under s_sock_mutex) so a stalled tx can never write into the new socket. */
            if (xSemaphoreTake(s_sock_mutex, portMAX_DELAY) == pdTRUE)
            {
                s_sock = new_sock;
                s_conn_gen++;
                xSemaphoreGive(s_sock_mutex);
            }
        }
        xEventGroupClearBits(s_event_group, PORT_CLOSED_BIT | PORT_CONN_BIT);
        if (source_addr.ss_family == PF_INET)
        {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
            ESP_LOGI(TAG, "Datalog stream accepted ip: %s", addr_str);
        }
        xEventGroupSetBits(s_event_group, PORT_OPEN_BIT);
        xEventGroupWaitBits(s_event_group, PORT_CLOSED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
        xEventGroupClearBits(s_event_group, PORT_OPEN_BIT | PORT_CONN_BIT);
        ESP_LOGI(TAG, "Datalog stream disconnected");
        if (xSemaphoreTake(s_sock_mutex, portMAX_DELAY) == pdTRUE)
        {
            shutdown(s_sock, 0);
            close(s_sock);
            s_sock = -1;
            xSemaphoreGive(s_sock_mutex);
        }
    }
}

bool datalog_stream_client_connected(void)
{
    if (s_event_group != NULL)
    {
        return (xEventGroupGetBits(s_event_group) & PORT_OPEN_BIT) ? true : false;
    }
    return false;
}

uint32_t datalog_stream_rows_sent(void)
{
    return s_rows_sent;
}

uint32_t datalog_stream_rows_dropped(void)
{
    return s_rows_dropped;
}

int8_t datalog_stream_init(uint32_t port)
{
    if (s_event_group != NULL)
    {
        return 0;   /* already up */
    }
    s_port = port;

    /* Create EVERYTHING before publishing ANYTHING: the csv hook gates only on s_event_group,
     * so it must be assigned last, once every other handle is known-good. A half-armed module
     * (e.g. NULL mutex) would panic the csv writer task on the first hook call. */
    SemaphoreHandle_t sock_mutex = xSemaphoreCreateMutex();
    SemaphoreHandle_t hdr_mutex = xSemaphoreCreateMutex();
    uint8_t *storage = heap_caps_malloc(STREAM_BUF_SIZE + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StreamBufferHandle_t ring =
        (storage != NULL) ? xStreamBufferCreateStatic(STREAM_BUF_SIZE, 1, storage, &s_stream_struct) : NULL;
    EventGroupHandle_t events = xEventGroupCreateStatic(&s_event_group_buffer);

    static StackType_t *server_task_stack, *rx_task_stack, *tx_task_stack;
    static StaticTask_t server_task_buffer, rx_task_buffer, tx_task_buffer;
    server_task_stack = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rx_task_stack = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tx_task_stack = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (sock_mutex == NULL || hdr_mutex == NULL || ring == NULL || events == NULL ||
        server_task_stack == NULL || rx_task_stack == NULL || tx_task_stack == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate stream listener state");
        if (sock_mutex) vSemaphoreDelete(sock_mutex);
        if (hdr_mutex) vSemaphoreDelete(hdr_mutex);
        if (ring) vStreamBufferDelete(ring);
        if (storage) heap_caps_free(storage);
        if (server_task_stack) heap_caps_free(server_task_stack);
        if (rx_task_stack) heap_caps_free(rx_task_stack);
        if (tx_task_stack) heap_caps_free(tx_task_stack);
        return -1;
    }

    xEventGroupSetBits(events, PORT_CLOSED_BIT);

    /* Publish (event group LAST -- it is the hook's entry gate), then create the tasks;
     * equal-priority tasks can preempt the instant they are created (boot-race convention). */
    s_sock_mutex = sock_mutex;
    s_hdr_mutex = hdr_mutex;
    s_stream_storage = storage;
    s_stream = ring;
    s_event_group = events;

    TaskHandle_t server_handle = xTaskCreateStatic(
        datalog_stream_server_task, "dlstrm_srv", 4096, NULL, 5, server_task_stack, &server_task_buffer);
    TaskHandle_t rx_handle = xTaskCreateStatic(
        datalog_stream_rx_task, "dlstrm_rx", 4096, NULL, 5, rx_task_stack, &rx_task_buffer);
    TaskHandle_t tx_handle = xTaskCreateStatic(
        datalog_stream_tx_task, "dlstrm_tx", 4096, NULL, 5, tx_task_stack, &tx_task_buffer);

    if (server_handle == NULL || rx_handle == NULL || tx_handle == NULL)
    {
        ESP_LOGE(TAG, "Failed to create datalog stream tasks");
        return -1;
    }
    ESP_LOGI(TAG, "Datalog stream listener initialised on %lu", port);
    return 0;
}
