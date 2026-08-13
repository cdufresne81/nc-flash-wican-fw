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
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"

#include "event_log.h"
#include "wifi_diag.h"

static const char *TAG = "wifi_diag";

// ---- Tunables ----------------------------------------------------------------------------------

#define WD_SAMPLE_MS        1000    // sampler period. 1 Hz is enough to characterise a link and
                                    // cheap enough to run forever.
#define WD_RSSI_RING_N      120     // 2 minutes of RSSI history for the window's sparkline
#define WD_EVT_RING_N       24      // last N link events (associations, drops, bans)
#define WD_EVT_LINE_MAX     128
#define WD_TALLY_N          10      // distinct disconnect reasons tracked
// One finding, including its "what to do about it" sentence. Sized to the LONGEST rule text in
// wd_findings() at its worst-case argument widths -- the build enforces this rather than trusting
// it: -Werror=format-truncation= fails the compile if any snprintf into this buffer could truncate.
// If a new rule does not fit, prefer tightening its wording to raising this; these read better
// short, and every finding is copied N times onto the HTTP handler's stack (see WD_FINDINGS_N).
#define WD_FINDING_MAX      288
// The exact number of rules in wd_findings() that can fire simultaneously (power save, channel
// clash, weak signal, PHY rate, dominant reason, memory, link uptime, slow connect). The
// "nothing suspicious" line only fires when none of them did, so it needs no slot of its own.
// Not a round number on purpose: a rule added without widening this would be silently dropped.
#define WD_FINDINGS_N       8
#define WD_TASK_PRIO        2       // below csv writer (4) and poll_log (5): never steals hot cycles
#define WD_TASK_STACK       3584

// Re-emitting an identical disconnect reason to the shared event log at most this often. A device
// stuck in a reconnect loop can drop every few seconds; unthrottled that would rotate every other
// event out of events.log (128 KB x 4) and destroy the very timeline we need to correlate against.
// The RAM ring and the tally still record EVERY drop -- only the SD line is throttled, and the
// suppressed count is carried on the next line that does get through, so nothing is silently lost.
#define WD_EVL_REPEAT_MS    60000

// RSSI histogram buckets, strongest first. Boundaries are the practical ones for 2.4 GHz:
// -60 is comfortable, -70 starts costing rate, -80 is where 802.11n gives up.
#define WD_HIST_N           5

// PHY bitfield (wifi_ap_record_t exposes these as bitfields; flattened so the snapshot is copyable).
#define WD_PHY_11B          0x01
#define WD_PHY_11G          0x02
#define WD_PHY_11N          0x04
#define WD_PHY_LR           0x08

// ---- State -------------------------------------------------------------------------------------

// One lock for the whole module. Every section under it is a handful of scalar stores or one
// strlcpy -- never an esp_wifi call, never a format, never an httpd send.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// Live radio snapshot, refreshed by the sampler.
typedef struct {
    bool     sta_up;            // sampler found a live association this tick
    bool     ap_up;             // SoftAP running
    int8_t   rssi;
    uint8_t  channel;           // STA primary channel
    uint8_t  ap_channel;        // SoftAP channel
    uint8_t  authmode;
    uint8_t  phy;               // WD_PHY_* bitfield
    uint8_t  mode;              // wifi_mode_t
    uint8_t  ps;                // wifi_ps_type_t
    uint8_t  bssid[6];
    char     ssid[33];
} wd_snap_t;

static wd_snap_t s_snap;
static char s_ip[16] = "0.0.0.0";
static char s_fw[32] = "?";

// Embedded diagnostic page, injected by main (see wifi_diag_set_page). Borrowed, never freed.
static const uint8_t *s_page;
static size_t s_page_len;

// Rolling aggregates. Sampler-owned; read under s_lock by the HTTP handlers.
static uint32_t s_samples;              // sampler ticks that saw an initialised radio
static uint32_t s_samples_up;           // ...of which the STA was associated
static uint32_t s_samples_overlap;      // ...of which AP and STA were on different channels
static int32_t  s_rssi_sum;             // sum over s_samples_up
static int8_t   s_rssi_min = 0;
static int8_t   s_rssi_max = -127;
static uint32_t s_rssi_hist[WD_HIST_N];
static int8_t   s_rssi_ring[WD_RSSI_RING_N];
static uint32_t s_rssi_ring_head;       // monotonic; ring holds the last min(head, N) samples

static uint32_t s_int_free_min = UINT32_MAX;   // smallest internal free seen
static uint32_t s_int_block_min = UINT32_MAX;  // smallest largest-free-block seen

// Link lifecycle counters.
static uint32_t s_connects;
static uint32_t s_got_ips;
static uint32_t s_disconnects;
static uint32_t s_bans;
static int64_t  s_session_start_ms;     // 0 when down
static int64_t  s_connected_ms_total;
static uint32_t s_session_longest_s;
static uint32_t s_session_current_s;
static int64_t  s_attempt_start_ms;     // 0 when no attempt outstanding
static uint32_t s_ttc_last_ms;          // last time-to-connect (attempt -> got IP)
static uint64_t s_ttc_sum_ms;
static uint32_t s_ttc_n;
static uint8_t  s_last_reason;
static uint32_t s_last_reason_up_s;
static bool     s_had_disconnect;

static struct {
    uint8_t  reason;
    uint16_t count;
} s_tally[WD_TALLY_N];

// event_log throttle bookkeeping (see WD_EVL_REPEAT_MS).
static int64_t  s_evl_last_ms;
static uint8_t  s_evl_last_reason;
static uint32_t s_evl_suppressed;

// Event ring.
typedef struct {
    uint32_t seq;                   // 1-based; 0 = never written
    uint32_t up_s;                  // uptime seconds when it happened
    char     text[WD_EVT_LINE_MAX];
} wd_evt_t;

static wd_evt_t s_evt[WD_EVT_RING_N];
static uint32_t s_evt_head;

static bool s_inited;

// ---- Small helpers -----------------------------------------------------------------------------

static uint32_t wd_up_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static int64_t wd_up_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void wifi_diag_set_fw_version(const char *version)
{
    if (version != NULL && version[0] != '\0')
    {
        strlcpy(s_fw, version, sizeof(s_fw));
    }
}

void wifi_diag_set_page(const uint8_t *html, size_t len)
{
    s_page = html;      // plain stores; written once at boot, before httpd is up
    s_page_len = len;
}

const char *wifi_diag_reason_str(uint8_t reason)
{
    // Glossed in the terms a user can act on, not the terms 802.11 uses. The codes that actually
    // show up in the field on this product are the 200-block; the rest are kept short.
    switch (reason)
    {
        case 1:   return "unspecified";
        case 2:   return "auth expired (the AP aged this station out)";
        case 3:   return "auth left (the AP deauthenticated us)";
        case 4:   return "association expired (the AP heard nothing from us in time)";
        case 5:   return "AP is full (too many stations associated)";
        case 6:   return "class-2 frame from a non-authenticated station";
        case 7:   return "class-3 frame from a non-associated station";
        case 8:   return "association left (the AP disassociated us)";
        case 9:   return "association without authentication";
        case 13:  return "invalid information element";
        case 14:  return "MIC failure (corrupt or wrong key material)";
        case 15:  return "4-way handshake timeout (usually a wrong password)";
        case 16:  return "group key update timeout";
        case 17:  return "information element differs between handshake frames";
        case 18:  return "invalid group cipher";
        case 19:  return "invalid pairwise cipher";
        case 20:  return "invalid AKMP";
        case 23:  return "802.1X authentication failed";
        case 24:  return "cipher suite rejected by the AP";
        case 200: return "beacon timeout (the AP stopped being heard -- range, interference, or it went quiet)";
        case 201: return "no AP found (SSID not on air, out of range, or a 5 GHz-only band)";
        case 202: return "authentication failed";
        case 203: return "association failed";
        case 204: return "handshake timeout (the AP answered but the key exchange did not finish)";
        case 205: return "connection failed";
        case 206: return "AP TSF reset";
        case 207: return "roaming to another AP";
        default:  return "see esp_wifi_types.h for this code";
    }
}

static const char *wd_ps_str(uint8_t ps)
{
    switch (ps)
    {
        case WIFI_PS_NONE:       return "NONE (radio always on)";
        case WIFI_PS_MIN_MODEM:  return "MIN_MODEM (sleeps between beacons)";
        case WIFI_PS_MAX_MODEM:  return "MAX_MODEM (sleeps aggressively)";
        default:                 return "unknown";
    }
}

static const char *wd_mode_str(uint8_t mode)
{
    switch (mode)
    {
        case WIFI_MODE_NULL:  return "off";
        case WIFI_MODE_STA:   return "STA";
        case WIFI_MODE_AP:    return "AP";
        case WIFI_MODE_APSTA: return "AP+STA";
        default:              return "unknown";
    }
}

static const char *wd_auth_str(uint8_t a)
{
    switch (a)
    {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
        default:                        return "other";
    }
}

// "11b/g/n" from the flattened PHY bitfield. Buffer must hold at least 12 bytes.
static void wd_phy_str(uint8_t phy, char *out, size_t cap)
{
    out[0] = '\0';
    if (phy & WD_PHY_11B) strlcat(out, "b", cap);
    if (phy & WD_PHY_11G) strlcat(out, "g", cap);
    if (phy & WD_PHY_11N) strlcat(out, "n", cap);
    if (phy & WD_PHY_LR)  strlcat(out, "+LR", cap);
    if (out[0] == '\0')   strlcat(out, "?", cap);
}

static int wd_hist_bucket(int8_t rssi)
{
    if (rssi >= -55) return 0;
    if (rssi >= -65) return 1;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 3;
    return 4;
}

static const char *wd_hist_label(int i)
{
    switch (i)
    {
        case 0:  return ">= -55 dBm  excellent";
        case 1:  return "-56..-65    good";
        case 2:  return "-66..-75    workable";
        case 3:  return "-76..-85    weak";
        default: return "<= -86      unusable";
    }
}

// Mask an SSID for a report that will be pasted in public: keep the first and last character so the
// owner can still recognise which network it was, hide the middle, and state the true length (a
// length mismatch is how a trailing-space or homoglyph SSID typo gets spotted).
static void wd_mask_ssid(const char *ssid, char *out, size_t cap)
{
    size_t n = strlen(ssid);
    if (n == 0)      { strlcpy(out, "(none)", cap); return; }
    if (n <= 2)      { snprintf(out, cap, "** (%u chars)", (unsigned)n); return; }
    snprintf(out, cap, "%c***%c (%u chars)", ssid[0], ssid[n - 1], (unsigned)n);
}

// Keep the OUI (first three octets): it identifies the AP's chipset vendor, which is genuinely
// diagnostic, and is not specific to one household. Hide the rest, which is the unique part.
static void wd_mask_bssid(const uint8_t *b, char *out, size_t cap)
{
    snprintf(out, cap, "%02X:%02X:%02X:xx:xx:xx", b[0], b[1], b[2]);
}

// ---- Event ring --------------------------------------------------------------------------------

static void wd_evt_push(const char *fmt, ...)
{
    char text[WD_EVT_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    uint32_t up = wd_up_s();

    portENTER_CRITICAL(&s_lock);
    uint32_t idx = s_evt_head % WD_EVT_RING_N;
    strlcpy(s_evt[idx].text, text, sizeof(s_evt[idx].text));
    s_evt[idx].up_s = up;
    s_evt_head++;
    s_evt[idx].seq = s_evt_head;
    portEXIT_CRITICAL(&s_lock);
}

static void wd_tally_add(uint8_t reason)
{
    for (int i = 0; i < WD_TALLY_N; i++)
    {
        if (s_tally[i].count != 0 && s_tally[i].reason == reason)
        {
            if (s_tally[i].count < UINT16_MAX) s_tally[i].count++;
            return;
        }
    }
    for (int i = 0; i < WD_TALLY_N; i++)
    {
        if (s_tally[i].count == 0)
        {
            s_tally[i].reason = reason;
            s_tally[i].count = 1;
            return;
        }
    }
    // More than WD_TALLY_N distinct reasons: the tail is noise for this purpose, drop it. The total
    // disconnect count (s_disconnects) still counts it, so the report's totals stay honest.
}

// ---- Event hooks -------------------------------------------------------------------------------

void wifi_diag_note_attempt(const char *ssid)
{
    const char *s = (ssid != NULL) ? ssid : "";
    portENTER_CRITICAL(&s_lock);
    s_attempt_start_ms = wd_up_ms();
    portEXIT_CRITICAL(&s_lock);

    wd_evt_push("attempt   ssid='%s'", s);
    // Chatty by nature: a failing device retries every few seconds. Ring-only unless debug is on.
    EVENT_LOG_DEBUG(EVL_WIFI, "attempt ssid='%s'", s);
}

void wifi_diag_note_connected(const char *ssid, const uint8_t *bssid, uint8_t channel)
{
    const char *s = (ssid != NULL) ? ssid : "";
    char bs[20] = "??:??:??:??:??:??";
    if (bssid != NULL)
    {
        snprintf(bs, sizeof(bs), "%02X:%02X:%02X:%02X:%02X:%02X",
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    }

    portENTER_CRITICAL(&s_lock);
    s_connects++;
    portEXIT_CRITICAL(&s_lock);

    wd_evt_push("associate ssid='%s' bssid=%s ch=%u", s, bs, (unsigned)channel);
    event_log_emit(EVL_WIFI, "associated ssid='%s' ch=%u", s, (unsigned)channel);
}

void wifi_diag_note_got_ip(const char *ip)
{
    uint32_t ttc = 0;

    portENTER_CRITICAL(&s_lock);
    s_got_ips++;
    s_session_start_ms = wd_up_ms();
    if (s_attempt_start_ms != 0)
    {
        int64_t d = s_session_start_ms - s_attempt_start_ms;
        if (d < 0) d = 0;
        ttc = (uint32_t)d;
        s_ttc_last_ms = ttc;
        s_ttc_sum_ms += ttc;
        s_ttc_n++;
        s_attempt_start_ms = 0;
    }
    if (ip != NULL) strlcpy(s_ip, ip, sizeof(s_ip));
    portEXIT_CRITICAL(&s_lock);

    if (ttc != 0)
    {
        wd_evt_push("got IP    %s after %u ms", (ip != NULL) ? ip : "?", (unsigned)ttc);
        event_log_emit(EVL_WIFI, "got IP %s (connect took %u ms)",
                       (ip != NULL) ? ip : "?", (unsigned)ttc);
    }
    else
    {
        wd_evt_push("got IP    %s", (ip != NULL) ? ip : "?");
        event_log_emit(EVL_WIFI, "got IP %s", (ip != NULL) ? ip : "?");
    }
}

void wifi_diag_note_disconnected(const char *ssid, uint8_t reason)
{
    const char *s = (ssid != NULL) ? ssid : "";
    int64_t now = wd_up_ms();
    uint32_t held_s = 0;
    int8_t rssi_at_drop;
    bool link_was_up;
    bool emit_evl;
    uint32_t suppressed = 0;

    portENTER_CRITICAL(&s_lock);
    s_disconnects++;
    s_had_disconnect = true;
    s_last_reason = reason;
    s_last_reason_up_s = (uint32_t)(now / 1000);
    wd_tally_add(reason);

    if (s_session_start_ms != 0)
    {
        int64_t d = now - s_session_start_ms;
        if (d < 0) d = 0;
        s_connected_ms_total += d;
        held_s = (uint32_t)(d / 1000);
        if (held_s > s_session_longest_s) s_session_longest_s = held_s;
        s_session_start_ms = 0;
    }

    // The RSSI that matters is the last one read WHILE THE LINK WAS UP: esp_wifi_sta_get_ap_info()
    // fails the moment it drops, so reading it here would report nothing at all.
    rssi_at_drop = s_snap.rssi;
    link_was_up  = s_snap.sta_up;

    // Throttle only the shared-log line (see WD_EVL_REPEAT_MS).
    emit_evl = (reason != s_evl_last_reason) || (s_evl_last_ms == 0) ||
               ((now - s_evl_last_ms) >= WD_EVL_REPEAT_MS);
    if (emit_evl)
    {
        suppressed = s_evl_suppressed;
        s_evl_suppressed = 0;
        s_evl_last_ms = now;
        s_evl_last_reason = reason;
    }
    else
    {
        s_evl_suppressed++;
    }
    portEXIT_CRITICAL(&s_lock);

    if (link_was_up)
    {
        wd_evt_push("DROP      reason=%u rssi=%d held=%us ssid='%s'",
                    (unsigned)reason, (int)rssi_at_drop, (unsigned)held_s, s);
    }
    else
    {
        wd_evt_push("DROP      reason=%u (never associated) ssid='%s'", (unsigned)reason, s);
    }

    if (emit_evl)
    {
        // NOT gated behind EVENT_LOG_DEBUG: this is the forensic line. A drop that happened while
        // the car was moving and nobody was watching is exactly what this whole component exists to
        // record, and event_log.h is explicit that a gated-off line is gone for good.
        if (suppressed != 0)
        {
            event_log_emit(EVL_WIFI, "DROP reason=%u (%s) rssi=%d held=%us [+%u similar suppressed]",
                           (unsigned)reason, wifi_diag_reason_str(reason),
                           (int)rssi_at_drop, (unsigned)held_s, (unsigned)suppressed);
        }
        else
        {
            event_log_emit(EVL_WIFI, "DROP reason=%u (%s) rssi=%d held=%us",
                           (unsigned)reason, wifi_diag_reason_str(reason),
                           (int)rssi_at_drop, (unsigned)held_s);
        }
    }
}

void wifi_diag_note_ban(const char *ssid, uint32_t ms)
{
    const char *s = (ssid != NULL) ? ssid : "";
    portENTER_CRITICAL(&s_lock);
    s_bans++;
    portEXIT_CRITICAL(&s_lock);

    wd_evt_push("BAN       ssid='%s' for %us", s, (unsigned)(ms / 1000));
    // Always logged: a banned SSID is invisible everywhere else and presents to the user as an
    // unexplained refusal to connect.
    event_log_emit(EVL_WIFI, "SSID '%s' banned for %us after repeated auth failures",
                   s, (unsigned)(ms / 1000));
}

// ---- Sampler -----------------------------------------------------------------------------------

static void wd_sample_once(void)
{
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK)
    {
        return;   // radio not initialised yet: not a sample, don't count it
    }

    wd_snap_t snap = {0};
    snap.mode = (uint8_t)mode;

    wifi_ps_type_t ps = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&ps) == ESP_OK)
    {
        snap.ps = (uint8_t)ps;
    }

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
    {
        wifi_config_t apc;
        if (esp_wifi_get_config(WIFI_IF_AP, &apc) == ESP_OK)
        {
            snap.ap_up = true;
            snap.ap_channel = apc.ap.channel;
        }
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        snap.sta_up = true;
        snap.rssi = ap.rssi;
        snap.channel = ap.primary;
        snap.authmode = (uint8_t)ap.authmode;
        if (ap.phy_11b) snap.phy |= WD_PHY_11B;
        if (ap.phy_11g) snap.phy |= WD_PHY_11G;
        if (ap.phy_11n) snap.phy |= WD_PHY_11N;
        if (ap.phy_lr)  snap.phy |= WD_PHY_LR;
        memcpy(snap.bssid, ap.bssid, 6);
        strlcpy(snap.ssid, (const char *)ap.ssid, sizeof(snap.ssid));
    }

    // Internal-RAM headroom. WiFi TX buffers come from internal RAM
    // (CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=y), so the largest *contiguous* block is what actually
    // gates transmit -- a large total free split into small fragments still starves the driver.
    // heap_caps_get_largest_free_block() walks the free lists under the heap lock; at 1 Hz on a
    // priority-2 task that is negligible, but it is why this must never move onto a hot path.
    uint32_t ifree = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t iblock = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    bool overlap = snap.sta_up && snap.ap_up && snap.ap_channel != 0 &&
                   snap.channel != 0 && snap.ap_channel != snap.channel;

    portENTER_CRITICAL(&s_lock);
    s_snap = snap;
    s_samples++;
    if (ifree < s_int_free_min)   s_int_free_min = ifree;
    if (iblock < s_int_block_min) s_int_block_min = iblock;
    if (overlap) s_samples_overlap++;
    if (snap.sta_up)
    {
        s_samples_up++;
        s_rssi_sum += snap.rssi;
        if (s_rssi_min == 0 || snap.rssi < s_rssi_min) s_rssi_min = snap.rssi;
        if (snap.rssi > s_rssi_max) s_rssi_max = snap.rssi;
        s_rssi_hist[wd_hist_bucket(snap.rssi)]++;
        s_rssi_ring[s_rssi_ring_head % WD_RSSI_RING_N] = snap.rssi;
        s_rssi_ring_head++;
        if (s_session_start_ms != 0)
        {
            int64_t d = wd_up_ms() - s_session_start_ms;
            s_session_current_s = (d > 0) ? (uint32_t)(d / 1000) : 0;
        }
    }
    else
    {
        s_session_current_s = 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void wd_sampler_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        wd_sample_once();
        vTaskDelay(pdMS_TO_TICKS(WD_SAMPLE_MS));
    }
}

void wifi_diag_init(void)
{
    if (s_inited)
    {
        return;
    }
    s_inited = true;

    if (xTaskCreate(wd_sampler_task, "wifi_diag", WD_TASK_STACK, NULL, WD_TASK_PRIO, NULL) != pdPASS)
    {
        // Not fatal and deliberately not retried: the event hooks (the forensic half) work without
        // the sampler, so a failure here costs the aggregates, not the disconnect record.
        ESP_LOGE(TAG, "sampler task create failed - link events still recorded, aggregates disabled");
        return;
    }
    ESP_LOGI(TAG, "wifi diagnostics started (%d Hz sampler, %d-event ring)",
             1000 / WD_SAMPLE_MS, WD_EVT_RING_N);
}

// ---- Findings ----------------------------------------------------------------------------------

typedef struct {
    char sev;                       // 'E' problem, 'W' suspicious, 'I' informational
    char text[WD_FINDING_MAX];
} wd_finding_t;

// Rule-based reading of the aggregates. This is what turns the page from a data dump into a
// troubleshooting tool: every rule states what was observed AND what it means for the symptom.
// Deliberately conservative -- a false "everything is fine" is recoverable, a confident wrong
// diagnosis sends someone off for a day.
static int wd_findings(wd_finding_t *out, int max)
{
    // Snapshot everything under one lock, then reason about it outside.
    wd_snap_t snap;
    uint32_t samples, samples_up, overlap, disconnects, connects, ttc_n, block_min;
    int32_t rssi_sum;
    uint64_t ttc_sum;
    struct { uint8_t reason; uint16_t count; } tally[WD_TALLY_N];

    portENTER_CRITICAL(&s_lock);
    snap = s_snap;
    samples = s_samples;
    samples_up = s_samples_up;
    overlap = s_samples_overlap;
    disconnects = s_disconnects;
    connects = s_connects;
    rssi_sum = s_rssi_sum;
    ttc_sum = s_ttc_sum_ms;
    ttc_n = s_ttc_n;
    block_min = s_int_block_min;
    memcpy(tally, s_tally, sizeof(tally));
    portEXIT_CRITICAL(&s_lock);

    int n = 0;
    #define WD_ADD(severity, ...) do {                                   \
        if (n < max) {                                                   \
            out[n].sev = (severity);                                     \
            snprintf(out[n].text, sizeof(out[n].text), __VA_ARGS__);     \
            n++;                                                         \
        }                                                                \
    } while (0)

    if (snap.ps != WIFI_PS_NONE)
    {
        WD_ADD('W', "Power save is %s. The radio sleeps between beacons, which costs throughput and "
                    "adds latency spikes. Manual Wi-Fi setup disables it; the SmartConnect path "
                    "never calls esp_wifi_set_ps(), so it runs on the IDF default.",
                    wd_ps_str(snap.ps));
    }

    if (samples > 0 && overlap * 2 > samples)
    {
        // A percentage is 0..100 by construction, but the compiler cannot know that from a uint32
        // division and must budget ten digits for it. Narrowing to uint8_t hands it the real range,
        // which is what keeps this line inside WD_FINDING_MAX instead of 13 characters over it.
        uint8_t pct = (uint8_t)(overlap * 100 / samples);
        WD_ADD('W', "The SoftAP is on channel %u while the station is on channel %u, for %u%% of the "
                    "time sampled. One radio cannot hold two channels: it time-slices between them, "
                    "which can cost more than half the throughput. Turning the access point off once "
                    "the station connects removes this.",
                    (unsigned)snap.ap_channel, (unsigned)snap.channel, (unsigned)pct);
    }

    if (samples_up >= 30)
    {
        int avg = (int)(rssi_sum / (int32_t)samples_up);
        if (avg <= -80)
        {
            WD_ADD('E', "Signal averaged %d dBm, which is at or past the edge of usable. Expect low "
                        "rates and beacon-timeout drops regardless of anything else on this page.", avg);
        }
        else if (avg <= -72)
        {
            WD_ADD('W', "Signal averaged %d dBm. That is workable but already costs rate; it is the "
                        "usual explanation for transfers that are slow but never fail.", avg);
        }
    }

    if (snap.sta_up && (snap.phy & WD_PHY_11B) && !(snap.phy & (WD_PHY_11G | WD_PHY_11N)))
    {
        WD_ADD('W', "The link negotiated 802.11b only, which caps it at 11 Mbit/s before overhead. "
                    "Check whether the access point is forcing a legacy or mixed-b mode.");
    }
    else if (snap.sta_up && !(snap.phy & WD_PHY_11N))
    {
        WD_ADD('I', "802.11n was not negotiated, so frame aggregation (AMPDU) is not in use. "
                    "Throughput will be well below what the signal strength alone would allow.");
    }

    // Dominant disconnect reason: only interesting once it is both repeated and the clear majority.
    if (disconnects >= 3)
    {
        int best = -1;
        for (int i = 0; i < WD_TALLY_N; i++)
        {
            if (tally[i].count == 0) continue;
            if (best < 0 || tally[i].count > tally[best].count) best = i;
        }
        if (best >= 0 && tally[best].count >= 3 && tally[best].count * 2 >= disconnects)
        {
            WD_ADD('W', "%u of %u disconnects were reason %u -- %s. A repeating single cause is a "
                        "real fault, not bad luck.",
                        (unsigned)tally[best].count, (unsigned)disconnects,
                        (unsigned)tally[best].reason, wifi_diag_reason_str(tally[best].reason));
        }
    }

    if (block_min != UINT32_MAX && block_min < 32768)
    {
        WD_ADD('W', "Largest contiguous internal-RAM block fell to %u bytes. Wi-Fi transmit buffers "
                    "are allocated from internal RAM on demand, so once this gets low the driver "
                    "starts failing transmits and throughput collapses in bursts.",
                    (unsigned)block_min);
    }

    if (samples >= 120 && samples_up * 100 / samples < 90)
    {
        WD_ADD('W', "The station was associated for only %u%% of the %u seconds sampled. This is a "
                    "connection-stability problem, not a throughput one -- read the disconnect "
                    "reasons above first.",
                    (unsigned)(samples_up * 100 / samples), (unsigned)samples);
    }

    if (ttc_n > 0 && (ttc_sum / ttc_n) > 10000)
    {
        WD_ADD('I', "Connecting takes %u ms on average. Time is going into scan/auth/DHCP rather "
                    "than the link itself; a fixed channel on the access point usually helps.",
                    (unsigned)(ttc_sum / ttc_n));
    }

    if (n == 0)
    {
        if (samples < 60)
        {
            WD_ADD('I', "Only %u seconds sampled so far. Leave this running while the problem "
                        "happens, then look again -- most of these checks need a few minutes of "
                        "history before they can say anything.", (unsigned)samples);
        }
        else
        {
            WD_ADD('I', "Nothing suspicious found: signal, channel plan, power save, link stability "
                        "and memory headroom all look healthy over %u seconds and %u connect(s).",
                        (unsigned)samples, (unsigned)connects);
        }
    }
    #undef WD_ADD
    return n;
}

// ---- Chunked output helper ----------------------------------------------------------------------

// Accumulates formatted text and flushes it as httpd chunks. One 1 KB internal-RAM buffer instead of
// a chunk per line: the report is ~50 lines, and 50 tiny TCP writes over a marginal link is exactly
// the behaviour this component exists to diagnose.
typedef struct {
    httpd_req_t *req;
    char        *buf;
    size_t       cap;
    size_t       len;
    bool         failed;
} wd_out_t;

static void wd_out_flush(wd_out_t *o)
{
    if (o->failed || o->len == 0) return;
    if (httpd_resp_send_chunk(o->req, o->buf, o->len) != ESP_OK)
    {
        o->failed = true;
    }
    o->len = 0;
}

static void wd_pf(wd_out_t *o, const char *fmt, ...)
{
    if (o->failed) return;
    // Flush early rather than risk a truncated line: every caller below fits in 512 bytes, except
    // findings, which are bounded by WD_FINDING_MAX.
    if (o->cap - o->len < WD_FINDING_MAX + 64)
    {
        wd_out_flush(o);
        if (o->failed) return;
    }
    size_t room = o->cap - o->len;
    if (room < 2) return;               // cannot happen after the flush above; cheap belt-and-braces
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(o->buf + o->len, room, fmt, ap);
    va_end(ap);
    if (w > 0)
    {
        // vsnprintf returns what it WOULD have written. On truncation only room-1 bytes landed.
        o->len += ((size_t)w < room) ? (size_t)w : (room - 1);
    }
}

// ---- GET /wifi_diag/report ----------------------------------------------------------------------

static esp_err_t wd_send_report(httpd_req_t *req)
{
    // ?raw=1 unmasks the SSID/BSSID. Masked by default: the entire point of this report is that it
    // gets pasted into a public issue, and a default that leaks the user's network name would be
    // discovered by the first person it happened to, not before.
    bool raw = false;
    {
        const char *q = strchr(req->uri, '?');
        if (q != NULL && strstr(q, "raw=1") != NULL) raw = true;
    }

    // Floor at 512, not 256: wd_pf() flushes whenever fewer than WD_FINDING_MAX+64 bytes remain, so
    // a 256-byte buffer would flush before every single write and still risk truncating the longest
    // finding. 512 guarantees a full line always fits.
    size_t cap = 1024;
    char *buf = NULL;
    while (cap >= 512)
    {
        buf = heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (buf != NULL) break;
        cap /= 2;
    }
    if (buf == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    // Snapshot every number under one lock so the report is internally consistent.
    wd_snap_t snap;
    uint32_t samples, samples_up, overlap, connects, got_ips, disconnects, bans;
    uint32_t sess_cur, sess_long, ttc_last, ttc_n, ifree_min, iblock_min, last_reason_up;
    uint32_t hist[WD_HIST_N];
    int32_t  rssi_sum;
    int8_t   rssi_min, rssi_max;
    uint64_t ttc_sum;
    int64_t  connected_total;
    uint8_t  last_reason;
    bool     had_disc;
    char     ip[16];
    struct { uint8_t reason; uint16_t count; } tally[WD_TALLY_N];

    portENTER_CRITICAL(&s_lock);
    snap = s_snap;
    samples = s_samples;          samples_up = s_samples_up;      overlap = s_samples_overlap;
    connects = s_connects;        got_ips = s_got_ips;            disconnects = s_disconnects;
    bans = s_bans;                sess_cur = s_session_current_s; sess_long = s_session_longest_s;
    ttc_last = s_ttc_last_ms;     ttc_sum = s_ttc_sum_ms;         ttc_n = s_ttc_n;
    ifree_min = s_int_free_min;   iblock_min = s_int_block_min;
    rssi_sum = s_rssi_sum;        rssi_min = s_rssi_min;          rssi_max = s_rssi_max;
    connected_total = s_connected_ms_total;
    last_reason = s_last_reason;  last_reason_up = s_last_reason_up_s;  had_disc = s_had_disconnect;
    memcpy(hist, s_rssi_hist, sizeof(hist));
    memcpy(tally, s_tally, sizeof(tally));
    strlcpy(ip, s_ip, sizeof(ip));
    portEXIT_CRITICAL(&s_lock);

    char ssid_disp[48], bssid_disp[24], phy[12];
    if (raw)
    {
        snprintf(ssid_disp, sizeof(ssid_disp), "%s", snap.ssid[0] ? snap.ssid : "(none)");
        snprintf(bssid_disp, sizeof(bssid_disp), "%02X:%02X:%02X:%02X:%02X:%02X",
                 snap.bssid[0], snap.bssid[1], snap.bssid[2],
                 snap.bssid[3], snap.bssid[4], snap.bssid[5]);
    }
    else
    {
        wd_mask_ssid(snap.ssid, ssid_disp, sizeof(ssid_disp));
        wd_mask_bssid(snap.bssid, bssid_disp, sizeof(bssid_disp));
    }
    wd_phy_str(snap.phy, phy, sizeof(phy));

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=wifi_report.txt");

    wd_out_t o = { .req = req, .buf = buf, .cap = cap, .len = 0, .failed = false };

    uint32_t up = wd_up_s();
    char now[24];
    {
        struct timeval tv;
        struct tm tm_now;
        gettimeofday(&tv, NULL);
        localtime_r(&tv.tv_sec, &tm_now);
        if ((tm_now.tm_year + 1900) >= 2020)
            strftime(now, sizeof(now), "%Y-%m-%d %H:%M:%S", &tm_now);
        else
            strlcpy(now, "unsynced", sizeof(now));
    }

    wd_pf(&o, "WiCAN Wi-Fi diagnostic report\n");
    wd_pf(&o, "=============================\n\n");
    wd_pf(&o, "firmware    : %s\n", s_fw);
    wd_pf(&o, "generated   : %s (uptime %uh %um %us)\n",
          now, (unsigned)(up / 3600), (unsigned)((up / 60) % 60), (unsigned)(up % 60));
    wd_pf(&o, "identifiers : %s\n\n", raw ? "SHOWN (?raw=1)" : "masked -- safe to paste in public");

    wd_pf(&o, "FINDINGS\n--------\n");
    {
        wd_finding_t f[WD_FINDINGS_N];
        int nf = wd_findings(f, WD_FINDINGS_N);
        for (int i = 0; i < nf; i++)
        {
            const char *tag = (f[i].sev == 'E') ? "[!!]" : (f[i].sev == 'W') ? "[! ]" : "[i ]";
            wd_pf(&o, "%s %s\n", tag, f[i].text);
        }
    }

    wd_pf(&o, "\nRADIO\n-----\n");
    wd_pf(&o, "mode          : %s\n", wd_mode_str(snap.mode));
    wd_pf(&o, "power save    : %s\n", wd_ps_str(snap.ps));
    wd_pf(&o, "station       : %s\n", snap.sta_up ? "associated" : "NOT associated");
    wd_pf(&o, "ssid          : %s\n", ssid_disp);
    wd_pf(&o, "bssid         : %s\n", bssid_disp);
    wd_pf(&o, "ip            : %s\n", ip);
    wd_pf(&o, "security      : %s\n", wd_auth_str(snap.authmode));
    wd_pf(&o, "phy           : 802.11%s\n", phy);
    wd_pf(&o, "sta channel   : %u\n", (unsigned)snap.channel);
    if (snap.ap_up)
    {
        wd_pf(&o, "ap channel    : %u\n", (unsigned)snap.ap_channel);
    }
    else
    {
        wd_pf(&o, "ap channel    : (access point off)\n");
    }
    wd_pf(&o, "channel clash : %s\n",
          (samples > 0 && overlap > 0) ? "YES -- see findings" : "no");

    wd_pf(&o, "\nSIGNAL (%u samples while associated)\n-----------------------------------\n",
          (unsigned)samples_up);
    if (samples_up > 0)
    {
        wd_pf(&o, "now / min / avg / max : %d / %d / %d / %d dBm\n",
              (int)snap.rssi, (int)rssi_min,
              (int)(rssi_sum / (int32_t)samples_up), (int)rssi_max);
        for (int i = 0; i < WD_HIST_N; i++)
        {
            unsigned pct = (unsigned)(hist[i] * 100 / samples_up);
            char bar[21];
            unsigned fill = pct / 5;
            for (unsigned b = 0; b < 20; b++) bar[b] = (b < fill) ? '#' : '.';
            bar[20] = '\0';
            wd_pf(&o, "  %-22s %s %3u%%\n", wd_hist_label(i), bar, pct);
        }
    }
    else
    {
        wd_pf(&o, "  (never associated -- no signal history)\n");
    }

    wd_pf(&o, "\nLINK STABILITY\n--------------\n");
    wd_pf(&o, "associations  : %u\n", (unsigned)connects);
    wd_pf(&o, "ip leases     : %u\n", (unsigned)got_ips);
    wd_pf(&o, "disconnects   : %u\n", (unsigned)disconnects);
    wd_pf(&o, "ssid bans     : %u\n", (unsigned)bans);
    wd_pf(&o, "uptime linked : %u%% of %u s sampled\n",
          (samples > 0) ? (unsigned)(samples_up * 100 / samples) : 0, (unsigned)samples);
    wd_pf(&o, "session now   : %u s (longest %u s, total linked %u s)\n",
          (unsigned)sess_cur, (unsigned)sess_long,
          (unsigned)(connected_total / 1000));
    if (ttc_n > 0)
    {
        wd_pf(&o, "connect time  : last %u ms, average %u ms over %u connect(s)\n",
              (unsigned)ttc_last, (unsigned)(ttc_sum / ttc_n), (unsigned)ttc_n);
    }
    if (had_disc)
    {
        wd_pf(&o, "last drop     : reason %u at uptime %u s -- %s\n",
              (unsigned)last_reason, (unsigned)last_reason_up, wifi_diag_reason_str(last_reason));
    }

    wd_pf(&o, "\nDISCONNECT REASONS\n------------------\n");
    {
        bool any = false;
        for (int i = 0; i < WD_TALLY_N; i++)
        {
            if (tally[i].count == 0) continue;
            any = true;
            wd_pf(&o, "  %5u x  reason %-3u  %s\n",
                  (unsigned)tally[i].count, (unsigned)tally[i].reason,
                  wifi_diag_reason_str(tally[i].reason));
        }
        if (!any) wd_pf(&o, "  (none -- the link has not dropped since boot)\n");
    }

    wd_pf(&o, "\nMEMORY (internal RAM -- where Wi-Fi TX buffers come from)\n"
              "--------------------------------------------------------\n");
    wd_pf(&o, "free now / lowest seen        : %u / %u bytes\n",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (ifree_min == UINT32_MAX) ? 0u : (unsigned)ifree_min);
    wd_pf(&o, "largest block now / lowest    : %u / %u bytes\n",
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
          (iblock_min == UINT32_MAX) ? 0u : (unsigned)iblock_min);

    wd_pf(&o, "\nSTACK CONFIGURATION (build-time, same on every unit)\n"
              "---------------------------------------------------\n");
    // Stated as a fact rather than a finding: it is not a fault, but it is a hard ceiling on
    // single-stream throughput and it is the first thing to check when a transfer is slow while
    // signal, channel and power save all look healthy.
    wd_pf(&o, "tcp send window   : %u bytes (CONFIG_LWIP_TCP_SND_BUF_DEFAULT)\n",
          (unsigned)CONFIG_LWIP_TCP_SND_BUF_DEFAULT);
    wd_pf(&o, "tcp recv window   : %u bytes (CONFIG_LWIP_TCP_WND_DEFAULT)\n",
          (unsigned)CONFIG_LWIP_TCP_WND_DEFAULT);
    wd_pf(&o, "  A window this size caps one TCP stream at roughly window/round-trip-time. At 20 ms\n"
              "  round trip that is about 2 Mbit/s no matter how strong the signal is.\n");

    wd_pf(&o, "\nEVENT TIMELINE (most recent last)\n---------------------------------\n");
    {
        uint32_t head;
        portENTER_CRITICAL(&s_lock);
        head = s_evt_head;
        portEXIT_CRITICAL(&s_lock);
        uint32_t count = (head < WD_EVT_RING_N) ? head : WD_EVT_RING_N;
        if (count == 0)
        {
            wd_pf(&o, "  (no link events recorded yet)\n");
        }
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t s = head - count + i;
            char line[WD_EVT_LINE_MAX];
            uint32_t up_s = 0;
            bool valid;
            portENTER_CRITICAL(&s_lock);
            valid = (s_evt[s % WD_EVT_RING_N].seq == s + 1);
            if (valid)
            {
                strlcpy(line, s_evt[s % WD_EVT_RING_N].text, sizeof(line));
                up_s = s_evt[s % WD_EVT_RING_N].up_s;
            }
            portEXIT_CRITICAL(&s_lock);
            if (!valid) continue;
            wd_pf(&o, "  [%6us] %s\n", (unsigned)up_s, line);
        }
    }

    wd_pf(&o, "\n--\nThis report covers the current uptime only. The persistent record of Wi-Fi\n"
              "drops -- which survives reboots and covers unattended driving -- is in the event\n"
              "log: download /event_log and look for WIFI lines.\n");

    wd_out_flush(&o);
    heap_caps_free(buf);
    if (o.failed)
    {
        return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ---- GET /wifi_diag (JSON for the UI window) -----------------------------------------------------

// Minimal JSON string escaper: the only externally-sourced string that reaches the JSON is the SSID,
// which is arbitrary bytes from the air and absolutely can contain a quote or a backslash.
static void wd_json_str(wd_out_t *o, const char *s)
{
    wd_pf(o, "\"");
    for (const char *p = s; *p != '\0'; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c == '"')       wd_pf(o, "\\\"");
        else if (c == '\\') wd_pf(o, "\\\\");
        else if (c < 0x20)  wd_pf(o, "\\u%04x", c);
        else                wd_pf(o, "%c", c);
    }
    wd_pf(o, "\"");
}

static esp_err_t wd_send_json(httpd_req_t *req)
{
    // Floor at 512, not 256: wd_pf() flushes whenever fewer than WD_FINDING_MAX+64 bytes remain, so
    // a 256-byte buffer would flush before every single write and still risk truncating the longest
    // finding. 512 guarantees a full line always fits.
    size_t cap = 1024;
    char *buf = NULL;
    while (cap >= 512)
    {
        buf = heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (buf != NULL) break;
        cap /= 2;
    }
    if (buf == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    wd_snap_t snap;
    uint32_t samples, samples_up, overlap, connects, got_ips, disconnects, bans;
    uint32_t sess_cur, sess_long, ttc_last, ttc_n, iblock_min;
    int32_t  rssi_sum;
    int8_t   rssi_min, rssi_max;
    uint64_t ttc_sum;
    uint8_t  last_reason;
    bool     had_disc;
    char     ip[16];
    struct { uint8_t reason; uint16_t count; } tally[WD_TALLY_N];

    portENTER_CRITICAL(&s_lock);
    snap = s_snap;
    samples = s_samples;       samples_up = s_samples_up;       overlap = s_samples_overlap;
    connects = s_connects;     got_ips = s_got_ips;             disconnects = s_disconnects;
    bans = s_bans;             sess_cur = s_session_current_s;  sess_long = s_session_longest_s;
    ttc_last = s_ttc_last_ms;  ttc_sum = s_ttc_sum_ms;          ttc_n = s_ttc_n;
    iblock_min = s_int_block_min;
    rssi_sum = s_rssi_sum;     rssi_min = s_rssi_min;           rssi_max = s_rssi_max;
    last_reason = s_last_reason; had_disc = s_had_disconnect;
    memcpy(tally, s_tally, sizeof(tally));
    strlcpy(ip, s_ip, sizeof(ip));
    portEXIT_CRITICAL(&s_lock);

    char phy[12];
    wd_phy_str(snap.phy, phy, sizeof(phy));

    httpd_resp_set_type(req, "application/json");
    wd_out_t o = { .req = req, .buf = buf, .cap = cap, .len = 0, .failed = false };

    wd_pf(&o, "{\"uptime_s\":%u,\"fw\":", (unsigned)wd_up_s());
    wd_json_str(&o, s_fw);

    // The window renders on the user's own device showing their own network, so nothing is masked
    // here -- masking would defeat the most common use, spotting that it joined the wrong SSID.
    wd_pf(&o, ",\"radio\":{\"mode\":");
    wd_json_str(&o, wd_mode_str(snap.mode));
    wd_pf(&o, ",\"ps\":%u,\"ps_str\":", (unsigned)snap.ps);
    wd_json_str(&o, wd_ps_str(snap.ps));
    wd_pf(&o, ",\"sta_up\":%s,\"ssid\":", snap.sta_up ? "true" : "false");
    wd_json_str(&o, snap.ssid);
    wd_pf(&o, ",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\"",
          snap.bssid[0], snap.bssid[1], snap.bssid[2],
          snap.bssid[3], snap.bssid[4], snap.bssid[5]);
    wd_pf(&o, ",\"ip\":");
    wd_json_str(&o, ip);
    wd_pf(&o, ",\"auth\":");
    wd_json_str(&o, wd_auth_str(snap.authmode));
    wd_pf(&o, ",\"phy\":");
    wd_json_str(&o, phy);
    wd_pf(&o, ",\"channel\":%u,\"ap_up\":%s,\"ap_channel\":%u,\"overlap\":%s}",
          (unsigned)snap.channel, snap.ap_up ? "true" : "false",
          (unsigned)snap.ap_channel, (overlap > 0) ? "true" : "false");

    wd_pf(&o, ",\"signal\":{\"now\":%d,\"min\":%d,\"max\":%d,\"avg\":%d,\"samples\":%u",
          (int)snap.rssi, (int)rssi_min, (int)rssi_max,
          (samples_up > 0) ? (int)(rssi_sum / (int32_t)samples_up) : 0,
          (unsigned)samples_up);
    wd_pf(&o, ",\"history\":[");
    {
        uint32_t head;
        portENTER_CRITICAL(&s_lock);
        head = s_rssi_ring_head;
        portEXIT_CRITICAL(&s_lock);
        uint32_t count = (head < WD_RSSI_RING_N) ? head : WD_RSSI_RING_N;
        for (uint32_t i = 0; i < count; i++)
        {
            int8_t v;
            portENTER_CRITICAL(&s_lock);
            v = s_rssi_ring[(head - count + i) % WD_RSSI_RING_N];
            portEXIT_CRITICAL(&s_lock);
            wd_pf(&o, "%s%d", (i == 0) ? "" : ",", (int)v);
        }
    }
    wd_pf(&o, "]}");

    wd_pf(&o, ",\"link\":{\"connects\":%u,\"got_ips\":%u,\"disconnects\":%u,\"bans\":%u",
          (unsigned)connects, (unsigned)got_ips, (unsigned)disconnects, (unsigned)bans);
    wd_pf(&o, ",\"uptime_pct\":%u,\"samples\":%u,\"session_s\":%u,\"longest_s\":%u",
          (samples > 0) ? (unsigned)(samples_up * 100 / samples) : 0,
          (unsigned)samples, (unsigned)sess_cur, (unsigned)sess_long);
    wd_pf(&o, ",\"ttc_last_ms\":%u,\"ttc_avg_ms\":%u",
          (unsigned)ttc_last, (ttc_n > 0) ? (unsigned)(ttc_sum / ttc_n) : 0);
    wd_pf(&o, ",\"last_reason\":%d,\"last_reason_str\":",
          had_disc ? (int)last_reason : -1);
    wd_json_str(&o, had_disc ? wifi_diag_reason_str(last_reason) : "");
    wd_pf(&o, ",\"reasons\":[");
    {
        bool first = true;
        for (int i = 0; i < WD_TALLY_N; i++)
        {
            if (tally[i].count == 0) continue;
            wd_pf(&o, "%s{\"reason\":%u,\"count\":%u,\"text\":",
                  first ? "" : ",", (unsigned)tally[i].reason, (unsigned)tally[i].count);
            wd_json_str(&o, wifi_diag_reason_str(tally[i].reason));
            wd_pf(&o, "}");
            first = false;
        }
    }
    wd_pf(&o, "]}");

    wd_pf(&o, ",\"mem\":{\"int_free\":%u,\"int_block\":%u,\"int_block_min\":%u}",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
          (iblock_min == UINT32_MAX) ? 0u : (unsigned)iblock_min);

    wd_pf(&o, ",\"findings\":[");
    {
        wd_finding_t f[WD_FINDINGS_N];
        int nf = wd_findings(f, WD_FINDINGS_N);
        for (int i = 0; i < nf; i++)
        {
            wd_pf(&o, "%s{\"sev\":\"%c\",\"text\":", (i == 0) ? "" : ",", f[i].sev);
            wd_json_str(&o, f[i].text);
            wd_pf(&o, "}");
        }
    }
    wd_pf(&o, "]");

    wd_pf(&o, ",\"events\":[");
    {
        uint32_t head;
        portENTER_CRITICAL(&s_lock);
        head = s_evt_head;
        portEXIT_CRITICAL(&s_lock);
        uint32_t count = (head < WD_EVT_RING_N) ? head : WD_EVT_RING_N;
        bool first = true;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t s = head - count + i;
            char line[WD_EVT_LINE_MAX];
            uint32_t up_s = 0;
            bool valid;
            portENTER_CRITICAL(&s_lock);
            valid = (s_evt[s % WD_EVT_RING_N].seq == s + 1);
            if (valid)
            {
                strlcpy(line, s_evt[s % WD_EVT_RING_N].text, sizeof(line));
                up_s = s_evt[s % WD_EVT_RING_N].up_s;
            }
            portEXIT_CRITICAL(&s_lock);
            if (!valid) continue;
            wd_pf(&o, "%s{\"up_s\":%u,\"text\":", first ? "" : ",", (unsigned)up_s);
            wd_json_str(&o, line);
            wd_pf(&o, "}");
            first = false;
        }
    }
    wd_pf(&o, "]}");

    wd_out_flush(&o);
    heap_caps_free(buf);
    if (o.failed)
    {
        return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ---- Routing -------------------------------------------------------------------------------------

static esp_err_t wd_router_handler(httpd_req_t *req)
{
    const char *seg = req->uri + strlen("/wifi_diag");
    if (*seg == '/') seg++;

    char route[16] = {0};
    size_t rl = strcspn(seg, "?");
    if (rl >= sizeof(route)) rl = sizeof(route) - 1;
    memcpy(route, seg, rl);
    route[rl] = '\0';

    if (strcmp(route, "report") == 0)
    {
        return wd_send_report(req);
    }
    if (strcmp(route, "page") == 0)
    {
        if (s_page == NULL || s_page_len == 0)
        {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Diagnostic page not embedded");
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, "text/html");
        // No-store: this page is a live instrument. A cached copy served after a reboot would show
        // pre-reboot numbers under a fresh timestamp, which is worse than showing nothing.
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, (const char *)s_page, s_page_len);
    }
    return wd_send_json(req);
}

esp_err_t wifi_diag_register_handlers(httpd_handle_t server)
{
    static const httpd_uri_t wd_uri = {
        .uri = "/wifi_diag*",
        .method = HTTP_GET,
        .handler = wd_router_handler,
        .user_ctx = NULL,
    };
    esp_err_t ret = httpd_register_uri_handler(server, &wd_uri);
    if (ret == ESP_OK || ret == ESP_ERR_HTTPD_HANDLER_EXISTS)
    {
        return ESP_OK;
    }
    return ret;
}
