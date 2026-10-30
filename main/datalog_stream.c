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
 * Live datalog stream (issue #3). Wire protocol: datalog_stream.h.
 *
 * Two tasks touch this module, and each piece of state has one owner:
 *
 *   csv writer task -> stream_hook(). The ONLY producer. It owns the trip state (open or not,
 *                      file name, header copy) and decides every line that goes out, including
 *                      the sync block a new client needs. Lines go into s_ring as whole
 *                      messages, never blocking: a message either fits or is dropped.
 *   stream task     -> the ONLY consumer and the only task that touches a socket. It accepts
 *                      clients, drains s_ring to the connected one, and hangs up on errors.
 *
 * The one shared value is s_live_gen: 0 = no client, otherwise a number that changes on every
 * new connection. Every message carries the generation it was written for, and the stream task
 * drops any message whose generation is not the current one. That is what keeps rows meant for
 * a previous client (still in the ring when a new one connects) out of the new stream, without
 * a lock and without resetting the ring under a running producer.
 *
 * Memory: everything large is in PSRAM, on purpose. The first version of this feature kept a
 * 16 KB ring in internal RAM from boot, which left the trip logger unable to allocate its own
 * queue ("CSV start failed (out of memory)"). Only the writer task and the stream task read
 * these buffers, both ordinary tasks, and neither runs while the flash cache is off (SPI flash
 * writes stall every non-IRAM task). lwIP already keeps its own buffers in PSRAM on this build
 * (CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP), so the bytes were headed there anyway. Internal RAM
 * cost is a few hundred bytes of .bss.
 */

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "csv_logger.h"
#include "event_log.h"
#include "datalog_stream.h"

static const char *TAG = "DL_STREAM";

#ifndef GIT_SHA
#define GIT_SHA "unknown"
#endif

#define STREAM_RING_BYTES   (32 * 1024)   /* writer -> stream task, PSRAM                      */
#define STREAM_HDR_MAX      8192          /* header copy; the writer's own row buffer size    */
#define STREAM_LINE_MAX     128           /* "#session ..." and "#drop ..." lines              */
#define STREAM_MSG_HDR      5             /* per message: u32 generation + u8 kind            */
#define STREAM_MSG_MAX      (STREAM_MSG_HDR + STREAM_LINE_MAX * 2 + STREAM_HDR_MAX)

#define STREAM_TASK_STACK   6144          /* PSRAM; the task never touches flash               */
#define STREAM_TASK_PRIO    3             /* below the csv writer (4): streaming yields to SD  */
#define STREAM_POLL_MS      50            /* longest a hang-up or a new client goes unnoticed  */
#define STREAM_ACCEPT_POLL_MS 100         /* accept poll while no client is connected          */
#define STREAM_PING_MS      3000          /* "#ping" after this much silence, so the PC can tell
                                             a dead link (device asleep, rebooted, off Wi-Fi) from
                                             a quiet one: it never sends, so only data can tell */
#define STREAM_SEND_TIMEOUT_S 5           /* a client that stops reading is dropped after this */
#define KEEPALIVE_IDLE      5
#define KEEPALIVE_INTERVAL  5
#define KEEPALIVE_COUNT     3

enum { MSG_CTRL = 0, MSG_ROW = 1 };

/* ---- shared (one writer each, aligned 32-bit: atomic on the S3) ---- */
static volatile uint32_t s_live_gen = 0;       /* stream task writes                          */
static volatile uint32_t s_rows_sent = 0;      /* stream task writes                          */
static volatile uint32_t s_rows_dropped = 0;   /* writer task writes                          */

static MessageBufferHandle_t s_ring = NULL;
static StaticMessageBuffer_t s_ring_struct;

/* ---- csv writer task only ---- */
static bool     s_sess_active = false;
static char     s_sess_file[64];
static int      s_sess_cols = 0;
static char    *s_hdr = NULL;                   /* PSRAM, STREAM_HDR_MAX                       */
static size_t   s_hdr_len = 0;                  /* 0 = header unavailable -> #nohdr            */
static uint32_t s_served_gen = 0;               /* the client the writer last synced           */
static bool     s_sync_pending = false;         /* a state line still has to go out            */
static bool     s_sync_fresh = false;           /* ...and it is the client's first one         */
static uint32_t s_pending_drops = 0;            /* rows lost since the last message that fit   */
static uint8_t *s_msg = NULL;                   /* PSRAM, STREAM_MSG_MAX: message being built  */

/* ---- stream task only ---- */
static uint8_t *s_rx = NULL;                    /* PSRAM, STREAM_MSG_MAX                       */
static uint16_t s_port = 0;

/* Queue one message for client `gen`: an optional "#drop n" line, then a, then b. All or
 * nothing: on false nothing was queued and the caller decides what the loss means. */
static bool stream_put(uint32_t gen, uint8_t kind, const char *a, size_t alen,
                       const char *b, size_t blen)
{
    size_t len = STREAM_MSG_HDR;
    memcpy(s_msg, &gen, sizeof(gen));
    s_msg[4] = kind;
    if (s_pending_drops != 0)
    {
        int n = snprintf((char *)s_msg + len, STREAM_LINE_MAX, "#drop %lu\n",
                         (unsigned long)s_pending_drops);
        if (n > 0)
        {
            len += MIN((size_t)n, (size_t)STREAM_LINE_MAX - 1);
        }
    }
    if (len + alen + blen > STREAM_MSG_MAX)
    {
        return false;
    }
    memcpy(s_msg + len, a, alen);
    len += alen;
    if (b != NULL && blen != 0)
    {
        memcpy(s_msg + len, b, blen);
        len += blen;
    }
    if (xMessageBufferSend(s_ring, s_msg, len, 0) != len)
    {
        return false;
    }
    s_pending_drops = 0;
    return true;
}

/* Tell client `gen` where the log stands: the open trip and its header, or that none is open.
 * A client's first sync says "#idle"; a later one means the trip it was following ended. */
static bool stream_put_sync(uint32_t gen)
{
    if (!s_sess_active)
    {
        return s_sync_fresh ? stream_put(gen, MSG_CTRL, "#idle\n", 6, NULL, 0)
                            : stream_put(gen, MSG_CTRL, "#close\n", 7, NULL, 0);
    }
    static char line[STREAM_LINE_MAX];   /* static: the writer's stack is internal RAM and small */
    int n = snprintf(line, sizeof(line), "#session file=%s cols=%d\n", s_sess_file, s_sess_cols);
    if (n <= 0)
    {
        return false;
    }
    n = MIN(n, (int)sizeof(line) - 1);
    if (s_hdr_len != 0)
    {
        return stream_put(gen, MSG_CTRL, line, (size_t)n, s_hdr, s_hdr_len);
    }
    return stream_put(gen, MSG_CTRL, line, (size_t)n, "#nohdr\n", 7);
}

/* csv writer task only (csv_logger.h). Never blocks. */
static void stream_hook(csv_stream_event_t ev, const char *file, int cols,
                        const char *line, size_t len)
{
    switch (ev)
    {
    case CSV_STREAM_EV_OPEN:
    {
        const char *slash = (file != NULL) ? strrchr(file, '/') : NULL;
        strlcpy(s_sess_file, (slash != NULL) ? slash + 1 : ((file != NULL) ? file : ""),
                sizeof(s_sess_file));
        s_sess_cols = cols;
        s_sess_active = true;
        s_hdr_len = 0;
        if (line != NULL && len != 0 && len <= STREAM_HDR_MAX)
        {
            memcpy(s_hdr, line, len);
            s_hdr_len = len;
        }
        s_sync_pending = true;
        break;
    }
    case CSV_STREAM_EV_CLOSE:
        s_sess_active = false;
        s_hdr_len = 0;
        s_sync_pending = true;
        break;
    default:
        break;
    }

    const uint32_t gen = s_live_gen;
    if (gen == 0)
    {
        s_served_gen = 0;
        return;
    }
    if (gen != s_served_gen)
    {
        /* A new client: its stream starts with a full sync and owes it no earlier drops. */
        s_served_gen = gen;
        s_sync_pending = true;
        s_sync_fresh = true;
        s_pending_drops = 0;
    }
    /* A sync that does not fit is retried on every later call, and no row may overtake it:
     * a row sent ahead of its "#session" + header would be read as the header. */
    if (s_sync_pending && stream_put_sync(gen))
    {
        s_sync_pending = false;
        s_sync_fresh = false;
    }
    if (ev == CSV_STREAM_EV_ROW && line != NULL && len != 0)
    {
        if (s_sync_pending || !stream_put(gen, MSG_ROW, line, len, NULL, 0))
        {
            s_pending_drops++;
            s_rows_dropped++;
        }
    }
}

static int stream_send_all(int sock, const uint8_t *buf, size_t len)
{
    while (len > 0)
    {
        int w = send(sock, buf, len, 0);
        if (w < 0)
        {
            return -1;
        }
        buf += w;
        len -= (size_t)w;
    }
    return 0;
}

static int stream_open_listener(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(s_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    /* Retry forever: a netif that is not ready yet at boot must not kill the port for good. */
    for (;;)
    {
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (fd >= 0)
        {
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            /* Non-blocking: the stream task polls it between messages instead of select(),
             * which allocates from the internal heap on every call. */
            const int fl = fcntl(fd, F_GETFL, 0);
            if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(fd, 1) == 0 &&
                fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0)
            {
                ESP_LOGI(TAG, "listening on %u", s_port);
                return fd;
            }
            close(fd);
        }
        ESP_LOGE(TAG, "listener on %u failed: errno %d (retry)", s_port, errno);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Hang up. s_live_gen goes to 0 FIRST so the writer stops producing for this client. A client
 * replaced by a newer one is told so ("#bye replaced") and must not reconnect: two clients that
 * both reconnect would take turns forever, each swap writing two event-log lines to the SD. */
static void stream_drop_client(int *client, const char *why, bool replaced)
{
    s_live_gen = 0;
    if (replaced)
    {
        send(*client, "#bye replaced\n", 14, MSG_DONTWAIT);   /* best effort */
    }
    shutdown(*client, SHUT_RDWR);
    close(*client);
    *client = -1;
    event_log_emit(EVL_INFO, "live stream: client gone (%s), %lu rows sent, %lu dropped",
                   why, (unsigned long)s_rows_sent, (unsigned long)s_rows_dropped);
}

/* A waiting connection, or -1 when there is none (the listener is non-blocking). */
static int stream_try_accept(int listen_fd, struct sockaddr_in *peer)
{
    socklen_t peer_len = sizeof(*peer);
    int fd = accept(listen_fd, (struct sockaddr *)peer, &peer_len);
    if (fd < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        ESP_LOGE(TAG, "accept failed: errno %d", errno);
        vTaskDelay(pdMS_TO_TICKS(100));   /* e.g. out of sockets: do not spin */
    }
    return fd;
}

/* Set up a just-accepted client and say hello. Returns the fd, or -1 if it is already gone. */
static int stream_adopt_client(int fd, const struct sockaddr_in *peer, uint32_t *gen_counter)
{
    /* Sends block (bounded by SO_SNDTIMEO) whatever the listener's mode. */
    const int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
    {
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    }
    int one = 1, idle = KEEPALIVE_IDLE, intvl = KEEPALIVE_INTERVAL, cnt = KEEPALIVE_COUNT;
    struct timeval snd_to = { .tv_sec = STREAM_SEND_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
    /* Rows are small and frequent; without this, Nagle + the PC's delayed ACK bunch them. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    char hello[96];
    int n = snprintf(hello, sizeof(hello), "#hello NCDLv1 fw=%s\n", GIT_SHA);
    n = MIN(n, (int)sizeof(hello) - 1);
    if (n <= 0 || stream_send_all(fd, (const uint8_t *)hello, (size_t)n) != 0)
    {
        close(fd);
        return -1;
    }

    /* Publish only after the hello is out, so everything the writer queues for this client
     * lands behind it. 0 means "no client", so the counter skips it on wrap. */
    if (++*gen_counter == 0)
    {
        ++*gen_counter;
    }
    s_live_gen = *gen_counter;

    char ip[16];
    inet_ntoa_r(peer->sin_addr, ip, sizeof(ip));
    event_log_emit(EVL_INFO, "live stream: client connected (%s)", ip);

    /* With no writer task (logging disabled and never started) nobody else would say so. If the
     * writer starts right now its own sync simply follows this line, which is still true order. */
    if (!csv_logger_running() && stream_send_all(fd, (const uint8_t *)"#idle\n", 6) != 0)
    {
        stream_drop_client(&fd, "send failed", false);
    }
    return fd;
}

static void stream_task(void *arg)
{
    int listen_fd = stream_open_listener();
    int client = -1;
    uint32_t gen_counter = 0;
    TickType_t last_tx = 0;
    static uint8_t discard[64];

    for (;;)
    {
        /* A newer client wins: a PC that reconnects after a Wi-Fi blip must not wait for
         * keepalive to notice its own dead connection. */
        struct sockaddr_in peer;
        const int fresh = stream_try_accept(listen_fd, &peer);
        if (fresh >= 0)
        {
            if (client >= 0)
            {
                stream_drop_client(&client, "replaced by a new client", true);
            }
            client = stream_adopt_client(fresh, &peer, &gen_counter);
            last_tx = xTaskGetTickCount();
            continue;
        }
        if (client < 0)
        {
            vTaskDelay(pdMS_TO_TICKS(STREAM_ACCEPT_POLL_MS));
            continue;
        }

        size_t got = xMessageBufferReceive(s_ring, s_rx, STREAM_MSG_MAX, pdMS_TO_TICKS(STREAM_POLL_MS));
        if (got > STREAM_MSG_HDR)
        {
            uint32_t gen;
            memcpy(&gen, s_rx, sizeof(gen));
            if (gen == s_live_gen)   /* anything else was meant for an earlier client */
            {
                if (stream_send_all(client, s_rx + STREAM_MSG_HDR, got - STREAM_MSG_HDR) != 0)
                {
                    stream_drop_client(&client, (errno == EAGAIN || errno == EWOULDBLOCK)
                                                    ? "not reading" : "send failed", false);
                    continue;
                }
                last_tx = xTaskGetTickCount();
                if (s_rx[4] == MSG_ROW)
                {
                    s_rows_sent++;
                }
            }
        }

        /* Hung up? */
        const int r = recv(client, discard, sizeof(discard), MSG_DONTWAIT);
        if (r == 0)
        {
            stream_drop_client(&client, "closed by client", false);
            continue;
        }
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            stream_drop_client(&client, "connection lost", false);
            continue;
        }
        /* r > 0: the client should not send anything; ignore it. */

        /* Whole messages only ever go out from this task, so a ping can never split a line. */
        if ((xTaskGetTickCount() - last_tx) >= pdMS_TO_TICKS(STREAM_PING_MS))
        {
            if (stream_send_all(client, (const uint8_t *)"#ping\n", 6) != 0)
            {
                stream_drop_client(&client, "send failed", false);
                continue;
            }
            last_tx = xTaskGetTickCount();
        }
    }
}

int datalog_stream_init(void)
{
    if (s_ring != NULL)
    {
        return 0;
    }
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t *ring_mem = heap_caps_malloc(STREAM_RING_BYTES, caps);
    s_hdr = heap_caps_malloc(STREAM_HDR_MAX, caps);
    s_msg = heap_caps_malloc(STREAM_MSG_MAX, caps);
    s_rx = heap_caps_malloc(STREAM_MSG_MAX, caps);
    MessageBufferHandle_t ring = (ring_mem != NULL)
        ? xMessageBufferCreateStatic(STREAM_RING_BYTES, ring_mem, &s_ring_struct) : NULL;
    if (ring == NULL || s_hdr == NULL || s_msg == NULL || s_rx == NULL)
    {
        ESP_LOGE(TAG, "out of PSRAM; live stream disabled");
        heap_caps_free(ring_mem);
        heap_caps_free(s_hdr);
        heap_caps_free(s_msg);
        heap_caps_free(s_rx);
        s_hdr = NULL;
        s_msg = NULL;
        s_rx = NULL;
        return -1;
    }
    s_ring = ring;
    /* Registered last: the hook runs on the writer task the moment it is visible. */
    csv_logger_set_stream_hook(stream_hook);
    return 0;
}

int datalog_stream_start(uint16_t port)
{
    static StaticTask_t task_buf;
    static bool started = false;
    if (s_ring == NULL || started)
    {
        return (s_ring == NULL) ? -1 : 0;
    }
    StackType_t *stack = heap_caps_malloc(STREAM_TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stack == NULL)
    {
        ESP_LOGE(TAG, "no stack; live stream disabled");
        return -1;
    }
    s_port = port;
    if (xTaskCreateStatic(stream_task, "dl_stream", STREAM_TASK_STACK, NULL, STREAM_TASK_PRIO,
                          stack, &task_buf) == NULL)
    {
        heap_caps_free(stack);
        return -1;
    }
    started = true;
    return 0;
}

bool datalog_stream_client_connected(void)
{
    return s_live_gen != 0;
}

uint32_t datalog_stream_rows_sent(void)
{
    return s_rows_sent;
}

uint32_t datalog_stream_rows_dropped(void)
{
    return s_rows_dropped;
}
