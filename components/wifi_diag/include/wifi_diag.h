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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// WiFi link diagnostics (issue #105).
//
// Users report two symptoms in STA mode -- slow transfers and connections that drop -- and the
// firmware currently keeps no evidence of either. The disconnect reason code the IDF hands us is
// ESP_LOGW'd (wifi_mgr.c) and thrown away: this build compiles at CONFIG_LOG_MAXIMUM_LEVEL=INFO
// and the board's USB-C port is a USB *host* at runtime, so there is no serial console anyone can
// read. By the time a user notices "it dropped again", the only account of why is gone.
//
// This component keeps that account. Two independent sources, because the two symptoms fail
// differently:
//
//   1. A 1 Hz sampler task snapshots the live radio (RSSI, channel, PHY, power-save mode, AP/STA
//      channel overlap, internal-heap headroom) and folds it into rolling aggregates plus a short
//      RSSI ring. A single spot reading proves nothing -- "RSSI averaged -78 dBm over 20 minutes"
//      is a diagnosis.
//   2. Event hooks called from wifi_mgr's event handler record every association, IP lease and
//      disconnect with its reason code, the RSSI at the moment it happened (read from the sampler,
//      since esp_wifi_sta_get_ap_info() fails once the link is down), and how long the session
//      lasted. A per-reason tally turns "it drops sometimes" into "14 of 16 were BEACON_TIMEOUT".
//
// CONTRACT (mirrors event_log's, which this component leans on):
//  - LEAF component: requires only base IDF + event_log, so main/wifi_mgr.c and main/config_server.c
//    can both call in without a dependency cycle. event_log does not know this component exists.
//  - Every wifi_diag_note_*() entry is NON-BLOCKING and safe to call from the esp_event loop task:
//    it takes a brief critical section over the ring, allocates nothing, and never touches the SD
//    card or the network on the caller's thread.
//  - Safe to call before wifi_diag_init() -- the notes land in the ring; only the sampler is missing.
//
// PERSISTENCE is deliberately delegated to event_log rather than reimplemented. Milestone lines
// (association, disconnect-with-reason, IP lease) are emitted as EVL_WIFI, so they inherit the
// brick-safe SD writer, the RTC crash guard, the rotation and the retrieval endpoint that component
// already ships -- and land in the same timeline as the boot/sleep/datalog events they need to be
// correlated against. That matters for the drop-while-driving case, where nobody is holding a
// browser open. The 1 Hz samples are NOT persisted: at 86k lines a day they would drown the log
// they share, and their value is entirely in the aggregate this component keeps in RAM.

// ---- Injection (this is a leaf; main owns these facts) -----------------------------------------

// Firmware version string for the report header, copied on the way in. Without it a pasted report
// cannot be tied to a build, which is the first question anyone asks. Safe to never call.
void wifi_diag_set_fw_version(const char *version);

// The diagnostic page's HTML, which main embeds in the image (EMBED_FILES) and hands over as a
// pointer + length; nothing is copied. Routing lives in this component so all four /wifi_diag*
// routes are decided in one place, but the asset has to come from main -- a leaf component cannot
// reach main's embedded symbols. Safe to never call: /wifi_diag/page then answers 404 and the JSON
// and report routes are unaffected.
void wifi_diag_set_page(const uint8_t *html, size_t len);

// ---- Lifecycle ---------------------------------------------------------------------------------

// Start the 1 Hz sampler. Idempotent. Call after wifi_network_init(), so the first sample sees a
// live radio; calling earlier is harmless (samples that find no radio are skipped, not counted).
void wifi_diag_init(void);

// ---- Event hooks (called from wifi_mgr's event handler) -----------------------------------------

// A connection attempt is being made to `ssid`. Starts the time-to-connect stopwatch that
// wifi_diag_note_got_ip() stops -- the number that separates "slow to associate" (AP or auth
// problem) from "slow once associated" (throughput problem).
void wifi_diag_note_attempt(const char *ssid);

// Association succeeded (WIFI_EVENT_STA_CONNECTED). `bssid` is 6 bytes, may be NULL.
void wifi_diag_note_connected(const char *ssid, const uint8_t *bssid, uint8_t channel);

// DHCP lease acquired (IP_EVENT_STA_GOT_IP). Stops the time-to-connect stopwatch and opens the
// session clock.
void wifi_diag_note_got_ip(const char *ip);

// Link lost (WIFI_EVENT_STA_DISCONNECTED) with the IDF's reason code. Closes the session clock and
// folds `reason` into the tally. The RSSI recorded against this event is the sampler's last reading
// while the link was up, which is the reading that explains the drop.
void wifi_diag_note_disconnected(const char *ssid, uint8_t reason);

// wifi_mgr banned an SSID after repeated auth failures. Rare and always interesting: a banned SSID
// is invisible from the UI otherwise, and looks to the user exactly like "it won't connect".
void wifi_diag_note_ban(const char *ssid, uint32_t ms);

// ---- Reason decoding ---------------------------------------------------------------------------

// Plain-English gloss for an IDF wifi_err_reason_t, e.g. 200 -> "beacon timeout (AP stopped being
// heard -- range, interference or the AP going quiet)". Never NULL; unknown codes get a generic
// string. Exposed because the report, the JSON and the UI must all say the same thing.
const char *wifi_diag_reason_str(uint8_t reason);

// ---- HTTP ---------------------------------------------------------------------------------------

// Register GET /wifi_diag* on the running httpd. Idempotent. Must be called before the catch-all
// wildcard handler. Routes:
//   /wifi_diag          -> JSON snapshot + aggregates + findings (feeds the page)
//   /wifi_diag/page     -> the standalone diagnostic page (a real URL on purpose: the browser's
//                          own reload works, and it can be bookmarked or opened on a phone while
//                          someone else drives)
//   /wifi_diag/report   -> the paste-ready plain-text investigation report
//                          (?raw=1 to unmask SSID/BSSID -- masked by default, see below)
//
// The report masks the SSID and the tail of the BSSID by default because its whole purpose is to be
// pasted into a public issue. The pre-shared key is never read by this component at all, so no code
// path can leak it. The JSON does NOT mask: it renders on the user's own device, showing them their
// own network, and masking there would make the window useless for spotting a wrong-SSID mistake.
esp_err_t wifi_diag_register_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
