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

// Largest-contiguous-internal-block level below which the Wi-Fi driver starts struggling to get a
// transmit buffer (~1.6 KB each). Kept next to the other tunables because the UI paints the same
// threshold red and tools/webtest/wifi_diag.test.mjs pins the two together -- change one and that
// test fails by name rather than the page and the report quietly disagreeing on a live device.
#define WD_LOW_BLOCK_BYTES  4096

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

static struct {
    uint8_t  reason;
    uint16_t count;
} s_tally[WD_TALLY_N];

// event_log throttle bookkeeping (see WD_EVL_REPEAT_MS).
static int64_t  s_evl_last_ms;
static uint8_t  s_evl_last_reason;
static uint32_t s_evl_suppressed;

// Event ring.
//
// The SSID and BSSID are stored SEPARATELY from the formatted text, never baked into it. They are
// the two fields the report masks, and masking can only happen at render time -- the same event has
// to appear masked in the report (which gets pasted in public) and in full in the JSON (which the
// owner reads on their own device). An earlier version formatted them straight into `text` and the
// report dumped the ring verbatim, so every timeline line leaked the network name and the router's
// full MAC under a header promising the opposite. Keep identifiers out of `text`.
typedef struct {
    uint32_t seq;                   // 1-based; 0 = never written
    uint32_t up_s;                  // uptime seconds when it happened
    char     text[WD_EVT_LINE_MAX]; // the event, WITHOUT any identifier
    char     ssid[33];              // "" when the event carries none
    uint8_t  bssid[6];
    bool     has_bssid;
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

// `ssid` may be NULL/empty and `bssid` may be NULL. Neither may appear in `fmt` -- see wd_evt_t.
static void wd_evt_push(const char *ssid, const uint8_t *bssid, const char *fmt, ...)
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
    strlcpy(s_evt[idx].ssid, (ssid != NULL) ? ssid : "", sizeof(s_evt[idx].ssid));
    s_evt[idx].has_bssid = (bssid != NULL);
    if (bssid != NULL) memcpy(s_evt[idx].bssid, bssid, 6);
    s_evt[idx].up_s = up;
    s_evt_head++;
    s_evt[idx].seq = s_evt_head;
    portEXIT_CRITICAL(&s_lock);
}

// Render one ring entry as a display line. `raw` shows identifiers in full; otherwise they are
// masked exactly as the RADIO section masks them, so a report cannot disagree with its own header.
static void wd_evt_render(const wd_evt_t *e, bool raw, char *out, size_t cap)
{
    char id[64] = "";
    if (e->ssid[0] != '\0')
    {
        if (raw)
        {
            snprintf(id, sizeof(id), " ssid='%s'", e->ssid);
        }
        else
        {
            char m[48];
            wd_mask_ssid(e->ssid, m, sizeof(m));
            snprintf(id, sizeof(id), " ssid=%s", m);
        }
    }

    char bs[32] = "";
    if (e->has_bssid)
    {
        if (raw)
        {
            snprintf(bs, sizeof(bs), " bssid=%02X:%02X:%02X:%02X:%02X:%02X",
                     e->bssid[0], e->bssid[1], e->bssid[2], e->bssid[3], e->bssid[4], e->bssid[5]);
        }
        else
        {
            char m[24];
            wd_mask_bssid(e->bssid, m, sizeof(m));
            snprintf(bs, sizeof(bs), " bssid=%s", m);
        }
    }

    snprintf(out, cap, "%s%s%s", e->text, id, bs);
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

/* ⚠️ EVERY event_log_emit() IN THIS FILE RUNS ON THE ESP EVENT TASK'S STACK.
 *
 * These hooks are called from wifi_mgr's event handling (wifi_mgr.c), which runs on the system
 * event task -- CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE, not a stack this component controls.
 * One event_log_emit() costs roughly 800 bytes there: evl_vemit() alone puts 328 bytes of buffers
 * on the stack (detail[112] + ts[24] + line[192]) and then calls vsnprintf, localtime_r, strftime
 * and snprintf, whose newlib internals are not small.
 *
 * That stack was 2304 bytes and it was NOT enough. Observed 2026-08-18 on a unit whose WiFi was
 * retrying constantly: a hard boot loop, ~13 s after every boot, decoded from the crash reporter
 * as vApplicationStackOverflowHook -> esp_system_abort. It reproduced identically on two
 * different firmware versions, which is what proved it was not the change being tested at the
 * time. Raised to 4608 in sdkconfig.
 *
 * The trigger was the debug-gated line below (it fires on EVERY connection attempt, and a failing
 * device attempts constantly), but the ungated lines further down -- associated / got IP / DROP --
 * pay the same cost on the same stack and were already close to the edge with debug OFF.
 *
 * So: before adding another emit here, or making any of these fire more often, check the headroom.
 * If this ever needs to be cheap, the fix is to defer the formatting off this task, not to shave
 * the buffers. */

void wifi_diag_note_attempt(const char *ssid)
{
    const char *s = (ssid != NULL) ? ssid : "";
    portENTER_CRITICAL(&s_lock);
    s_attempt_start_ms = wd_up_ms();
    portEXIT_CRITICAL(&s_lock);

    wd_evt_push(s, NULL, "attempt  ");
    // Chatty by nature: a failing device retries every few seconds. Ring-only unless debug is on.
    char m[48];
    wd_mask_ssid(s, m, sizeof(m));
    EVENT_LOG_DEBUG(EVL_WIFI, "attempt ssid=%s", m);
}

void wifi_diag_note_connected(const char *ssid, const uint8_t *bssid, uint8_t channel)
{
    const char *s = (ssid != NULL) ? ssid : "";

    portENTER_CRITICAL(&s_lock);
    s_connects++;
    portEXIT_CRITICAL(&s_lock);

    wd_evt_push(s, bssid, "associate ch=%u", (unsigned)channel);
    // Masked, with no raw escape hatch. The report can offer ?raw=1 because the caller chooses
    // per request; a line written to the SD event log is permanent, and /event_log is served
    // unmasked with no query parameter -- this report's own footer sends people there to read the
    // WIFI lines. Masking only the newer, showier route while the persisted one published the
    // network name in full would make the "safe to paste in public" promise worthless.
    char m[48];
    wd_mask_ssid(s, m, sizeof(m));
    event_log_emit(EVL_WIFI, "associated ssid=%s ch=%u", m, (unsigned)channel);
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

    // The local IP is not masked anywhere: it is an RFC1918 address handed out by the user's own
    // router and says nothing about who or where they are, while being one of the more useful
    // things in a report (a 169.254 address is a whole diagnosis on its own).
    if (ttc != 0)
    {
        wd_evt_push(NULL, NULL, "got IP    %s after %u ms", (ip != NULL) ? ip : "?", (unsigned)ttc);
        event_log_emit(EVL_WIFI, "got IP %s (connect took %u ms)",
                       (ip != NULL) ? ip : "?", (unsigned)ttc);
    }
    else
    {
        wd_evt_push(NULL, NULL, "got IP    %s", (ip != NULL) ? ip : "?");
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
        wd_evt_push(s, NULL, "DROP      reason=%u rssi=%d held=%us",
                    (unsigned)reason, (int)rssi_at_drop, (unsigned)held_s);
    }
    else
    {
        wd_evt_push(s, NULL, "DROP      reason=%u (never associated)", (unsigned)reason);
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

    wd_evt_push(s, NULL, "BAN       for %us", (unsigned)(ms / 1000));
    // Always logged: a banned SSID is invisible everywhere else and presents to the user as an
    // unexplained refusal to connect. Masked for the same reason as the association line above.
    char m[48];
    wd_mask_ssid(s, m, sizeof(m));
    event_log_emit(EVL_WIFI, "SSID %s banned for %us after repeated auth failures",
                   m, (unsigned)(ms / 1000));
}

// ---- Sampler -----------------------------------------------------------------------------------

/* The floor under that headroom, in BYTES still free on sys_evt's stack (#112).
 *
 * 1024 is justified, not picked for comfort. One more event_log_emit() on this stack costs roughly
 * 800 B (see the WARNING block in "Event hooks" above), and interrupt entry saves context on the
 * RUNNING task's stack before switching to the interrupt stack, which needs a couple hundred more.
 * So below 1024 B free the honest statement is "the next log line no longer fits": one added emit
 * plus an ill-timed interrupt is a panic. Above it there is still room to react before the cliff.
 *
 * Deliberately tighter than the sleep task's 2048 (sleep_mode.c:768). That task's resume path runs
 * the whole WiFi bring-up and is expected to grow; this task's job is supposed to SHRINK (#111). */
#define WD_SYS_EVT_STACK_WARN_MIN_FREE 1024

/* Looked up once and cached, so the healthy case costs a pointer read rather than a task-list walk
 * every second. The default event loop task is created during startup and never exits, so the
 * handle stays valid for the whole boot. NULL until the lookup succeeds -- on the first sampler
 * ticks the loop may not exist yet, which is normal, not an error. */
static TaskHandle_t s_sys_evt = NULL;
static bool         s_sys_evt_stack_warned = false;

/* Read sys_evt's stack headroom and complain ONCE if it is near the edge (#112).
 *
 * This runs on the sampler task and never inside the hooks above, and that placement IS the
 * design. uxTaskGetStackHighWaterMark() returns a HISTORIC MINIMUM -- the closest the task has
 * ever come to the end of its stack, recovered by scanning the untouched fill pattern -- so
 * reading it a second later from another task yields exactly the same worst case as reading it at
 * the deepest moment. Reading is free. WARNING is not: emitting from inside a wifi_diag_note_*
 * hook would spend ~800 B on the very stack that just proved short, so the warning could cause the
 * overflow it warns about.
 *
 * Latched for one boot for the same reason documented at sleep_mode.c:1479-1484: the mark only
 * ever shrinks, so within one uptime the condition can never clear, and re-emitting would repeat
 * the same number every second forever. It re-arms on any reboot.
 *
 * NOT debug-gated, matching the sleep floor check (sleep_mode.c:1472-1475): a line reporting the
 * bad case must never be hidden behind a setting. This box has no serial console to confess on. */
static void wd_check_sys_evt_stack(void)
{
    if (s_sys_evt_stack_warned)
    {
        return;   // already said it; the number cannot improve within this boot
    }

    if (s_sys_evt == NULL)
    {
        s_sys_evt = xTaskGetHandle("sys_evt");   // the IDF default event loop task's real name
        if (s_sys_evt == NULL)
        {
            return;   // not up yet: try again next tick, no complaint
        }
    }

    const unsigned free_b =
        (unsigned)(uxTaskGetStackHighWaterMark(s_sys_evt) * sizeof(StackType_t));

    if (free_b < WD_SYS_EVT_STACK_WARN_MIN_FREE)
    {
        s_sys_evt_stack_warned = true;
        event_log_emit(EVL_WARN, "sys_evt stack low: %u B free (floor %u B)",
                       free_b, (unsigned)WD_SYS_EVT_STACK_WARN_MIN_FREE);
    }
}

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

    // Read the clock BEFORE taking the lock. Cheap as it is (a couple of systimer reads), it was
    // the one call left inside a critical section, against this file's own stated rule that the
    // lock holds nothing but scalar stores.
    int64_t now_ms = wd_up_ms();

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
            int64_t d = now_ms - s_session_start_ms;
            s_session_current_s = (d > 0) ? (uint32_t)(d / 1000) : 0;
            // Keep the maximum live rather than only closing it out on disconnect. Updating it
            // solely in the disconnect handler meant a device that never dropped reported
            // "longest 0 s" forever -- which reads as "it never stayed connected", the exact
            // opposite of what it means.
            if (s_session_current_s > s_session_longest_s)
            {
                s_session_longest_s = s_session_current_s;
            }
        }
    }
    else
    {
        s_session_current_s = 0;
    }
    portEXIT_CRITICAL(&s_lock);

    /* Outside the critical section on purpose: this can emit, and an event_log_emit() inside a
     * portENTER_CRITICAL block would hold a spinlock across the formatting. */
    wd_check_sys_evt_stack();
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

    // Calibrated against what the Wi-Fi driver actually needs, NOT against a round number. A TX
    // buffer is ~1.6 KB, so trouble starts when the largest contiguous block can no longer hold a
    // couple of them; below WD_LOW_BLOCK_BYTES is where that begins to bite.
    //
    // This first shipped as "< 32768", which was worse than wrong: a healthy unit in the field runs
    // with roughly 30 KB of internal RAM free IN TOTAL, so a 32 KB contiguous block is arithmetically
    // impossible and the rule fired on every device, forever. A finding that always fires carries no
    // information -- it just teaches people to ignore the findings panel. If this needs retuning
    // again, tune it against a measured device, not against intuition.
    if (block_min != UINT32_MAX && block_min < WD_LOW_BLOCK_BYTES)
    {
        WD_ADD('W', "Largest contiguous internal-RAM block fell to %u bytes, below the %u needed to "
                    "reliably hand the Wi-Fi driver transmit buffers. Expect throughput to collapse "
                    "in bursts rather than degrade smoothly.",
                    (unsigned)block_min, (unsigned)WD_LOW_BLOCK_BYTES);
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

// Allocate the chunk buffer both handlers stream through, laddering down under memory pressure and
// answering the request itself on failure. Sized in one place because the floor is not arbitrary:
// wd_pf() flushes whenever fewer than WD_FINDING_MAX+64 bytes remain, so a 256-byte buffer would
// flush before every single write AND still risk truncating the longest finding. 512 guarantees a
// full line always fits.
static char *wd_out_alloc(httpd_req_t *req, size_t *cap_out)
{
    size_t cap = 1024;
    while (cap >= 512)
    {
        char *buf = heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (buf != NULL)
        {
            *cap_out = cap;
            return buf;
        }
        cap /= 2;
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return NULL;
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

    size_t cap;
    char *buf = wd_out_alloc(req, &cap);
    if (buf == NULL)
    {
        return ESP_FAIL;   /* wd_out_alloc already answered the request */
    }

    // Snapshot every number under one lock so the report is internally consistent.
    wd_snap_t snap;
    uint32_t samples, samples_up, overlap, connects, got_ips, disconnects, bans;
    uint32_t sess_cur, sess_long, ttc_last, ttc_n, last_reason_up;
    uint32_t hist[WD_HIST_N];
    int32_t  rssi_sum;
    int8_t   rssi_min, rssi_max;
    uint64_t ttc_sum;
    int64_t  connected_total;
    uint8_t  last_reason;
    char     ip[16];
    struct { uint8_t reason; uint16_t count; } tally[WD_TALLY_N];

    portENTER_CRITICAL(&s_lock);
    snap = s_snap;
    samples = s_samples;          samples_up = s_samples_up;      overlap = s_samples_overlap;
    connects = s_connects;        got_ips = s_got_ips;            disconnects = s_disconnects;
    bans = s_bans;                sess_cur = s_session_current_s; sess_long = s_session_longest_s;
    ttc_last = s_ttc_last_ms;     ttc_sum = s_ttc_sum_ms;         ttc_n = s_ttc_n;
    rssi_sum = s_rssi_sum;        rssi_min = s_rssi_min;          rssi_max = s_rssi_max;
    // Add the session still open, for the same reason as s_session_longest_s above: this total is
    // only banked on disconnect, so without this a device that has never dropped reports having
    // been connected for zero seconds while it is connected right now.
    connected_total = s_connected_ms_total;
    if (s_session_start_ms != 0)
    {
        int64_t live = wd_up_ms() - s_session_start_ms;
        if (live > 0) connected_total += live;
    }
    last_reason = s_last_reason;  last_reason_up = s_last_reason_up_s;
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
    if (disconnects != 0)
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
    {
        // "now" is read while this request is in flight, so it includes the memory httpd and this
        // handler's own buffer are holding -- which is how the report could print a "lowest seen"
        // HIGHER than "now" and contradict itself. Fold the live reading into the minimum first:
        // it is a real observation, it just happens to be one the 1 Hz sampler never sees.
        uint32_t ifree_now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        uint32_t iblock_now = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        portENTER_CRITICAL(&s_lock);
        if (ifree_now < s_int_free_min)   s_int_free_min = ifree_now;
        if (iblock_now < s_int_block_min) s_int_block_min = iblock_now;
        uint32_t ifree_min = s_int_free_min;
        uint32_t iblock_min = s_int_block_min;
        portEXIT_CRITICAL(&s_lock);

        wd_pf(&o, "free now / lowest seen        : %u / %u bytes\n",
              (unsigned)ifree_now, (unsigned)ifree_min);
        wd_pf(&o, "largest block now / lowest    : %u / %u bytes\n",
              (unsigned)iblock_now, (unsigned)iblock_min);
        wd_pf(&o, "  (measured while serving this report, so it counts this request's own buffers)\n");
    }

    wd_pf(&o, "\nSTACK CONFIGURATION (build-time, same on every unit)\n"
              "---------------------------------------------------\n");
    // Stated as a fact rather than a finding: it is not a fault, but it is a hard ceiling on
    // single-stream throughput and it is the first thing to check when a transfer is slow while
    // signal, channel and power save all look healthy.
    wd_pf(&o, "tcp send window   : %u bytes (CONFIG_LWIP_TCP_SND_BUF_DEFAULT)\n",
          (unsigned)CONFIG_LWIP_TCP_SND_BUF_DEFAULT);
    wd_pf(&o, "tcp recv window   : %u bytes (CONFIG_LWIP_TCP_WND_DEFAULT)\n",
          (unsigned)CONFIG_LWIP_TCP_WND_DEFAULT);
    {
        // Computed, never written out as prose. This line used to say a flat "about 2 Mbit/s",
        // which was derived by hand from sdkconfig.esp32s3 (5744) -- the wrong file. The build
        // uses the committed sdkconfig, where the window is 20480, so the sentence understated the
        // real ceiling by more than 3x while sitting directly beneath the correct number.
        uint32_t bps = (uint32_t)CONFIG_LWIP_TCP_SND_BUF_DEFAULT * 8u * 50u;   /* 20 ms RTT */
        wd_pf(&o, "  A window this size caps one TCP stream at roughly window/round-trip-time:\n"
                  "  about %u.%02u Mbit/s at 20 ms round trip, no matter how strong the signal is.\n",
              (unsigned)(bps / 1000000u), (unsigned)((bps % 1000000u) / 10000u));
    }

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
            wd_evt_t e;
            bool valid;
            portENTER_CRITICAL(&s_lock);
            valid = (s_evt[s % WD_EVT_RING_N].seq == s + 1);
            if (valid) e = s_evt[s % WD_EVT_RING_N];
            portEXIT_CRITICAL(&s_lock);
            if (!valid) continue;
            // Masked unless ?raw=1, matching this report's own header. The timeline is the one
            // place identifiers could sneak past the mask, so it renders through the same helper.
            char line[WD_EVT_LINE_MAX + 96];
            wd_evt_render(&e, raw, line, sizeof(line));
            wd_pf(&o, "  [%6us] %s\n", (unsigned)e.up_s, line);
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
    size_t cap;
    char *buf = wd_out_alloc(req, &cap);
    if (buf == NULL)
    {
        return ESP_FAIL;   /* wd_out_alloc already answered the request */
    }

    wd_snap_t snap;
    uint32_t samples, samples_up, overlap, connects, got_ips, disconnects, bans;
    uint32_t sess_cur, sess_long, ttc_last, ttc_n, iblock_min;
    int32_t  rssi_sum;
    int8_t   rssi_min, rssi_max;
    uint64_t ttc_sum;
    uint8_t  last_reason;
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
    last_reason = s_last_reason;
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
        // One lock for the whole ring, not one per sample. It is 120 bytes; copying it in a single
        // critical section costs less than the 240 lock/unlock pairs the per-sample version took,
        // and this runs every 5 s for as long as the diagnostic page is left open.
        int8_t ring[WD_RSSI_RING_N];
        portENTER_CRITICAL(&s_lock);
        memcpy(ring, s_rssi_ring, sizeof(ring));
        portEXIT_CRITICAL(&s_lock);
        for (uint32_t i = 0; i < count; i++)
        {
            wd_pf(&o, "%s%d", (i == 0) ? "" : ",",
                  (int)ring[(head - count + i) % WD_RSSI_RING_N]);
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
          (disconnects != 0) ? (int)last_reason : -1);
    wd_json_str(&o, (disconnects != 0) ? wifi_diag_reason_str(last_reason) : "");
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
            wd_evt_t e;
            bool valid;
            portENTER_CRITICAL(&s_lock);
            valid = (s_evt[s % WD_EVT_RING_N].seq == s + 1);
            if (valid) e = s_evt[s % WD_EVT_RING_N];
            portEXIT_CRITICAL(&s_lock);
            if (!valid) continue;
            // Unmasked here, like the rest of the JSON: it renders on the owner's own device.
            char line[WD_EVT_LINE_MAX + 96];
            wd_evt_render(&e, true, line, sizeof(line));
            wd_pf(&o, "%s{\"up_s\":%u,\"text\":", first ? "" : ",", (unsigned)e.up_s);
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
