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
#include "esp_tls_crypto.h"
#include <esp_http_server.h>

/* Product-wide maximum logging rate: ONE ceiling shared by the polled sweep
 * (POLLLOG_MIN_SWEEP_MS), the CSV grid (csv_grid_period_ms), the broadcast-column
 * throttle (POLLLOG_BCAST_PERIOD_MS), and the csv_grid_hz validators. 100 Hz -> 10 ms
 * minimum period. Nothing on an NC powertrain bus carries more than 100 Hz of real
 * information, so this is the whole-product promise, enforced at each chokepoint.
 *
 * If one specific resource later cannot sustain this rate (e.g. the SD write path stalls
 * at 100 Hz), do NOT edit the call sites down: introduce a named sub-ceiling
 * (e.g. CSV_SD_MAX_HZ), set it lower, and add
 *     _Static_assert(CSV_SD_MAX_HZ <= WICAN_LOG_MAX_HZ, "...");
 * so the divergence is deliberate, named, and compile-checked instead of a magic number
 * rotting in one file. The web UI (main.js / homepage_full.html) hard-codes the same 100
 * in the input max + validator -- C cannot reach JS, so keep those two in sync by hand. */
#define WICAN_LOG_MAX_HZ		100u
#define WICAN_LOG_MIN_PERIOD_MS	(1000u / WICAN_LOG_MAX_HZ)	/* 10 ms; derived so the pair can't drift (assumes MAX_HZ divides 1000) */

#define AP_MODE				0
#define APSTA_MODE			1
#define SMARTCONNECT_MODE	2
#define BLESTA_MODE         3
#define STA_MODE            4

#define CAN_5K				0
#define CAN_10K				1
#define CAN_20K				2
#define CAN_25K				3
#define CAN_50K				4
#define CAN_100K			5
#define CAN_125K			6
#define CAN_250K			7
#define CAN_500K			8
#define CAN_800K			9
#define CAN_1000K			10

#define CAN_NORMAL			0
#define CAN_SILENT			1

#define UDP_PORT			0
#define TCP_PORT			1

/* Fixed TCP port of the dedicated always-on SLCAN listener used by NC Flash to reach the ECU
 * without any protocol switch; MUST match the host's WICAN_DEDICATED_SLCAN_PORT
 * (src/ecu/constants.py). Lives here rather than in main.c so any module can name the port
 * without redefining the number. It is the ONLY TCP port this firmware opens for CAN traffic:
 * the configurable stock server is gone, so nothing can collide with it any more. */
#define WICAN_DEDICATED_SLCAN_PORT	35001

/* protocol ids 0 (slcan), 1 (realdash66) and 2 (savvycan) retired; values reserved so the
 * remaining ids never shift. id 0 went when `protocol` became a placebo: the device has one
 * mode and the stored value is discarded at the parse site, so no id here is selectable any
 * more. The rest stay defined because dead-but-compiled router arms in main.c still name them. */
#define OBD_ELM327			3
#define AUTO_PID			4
#define FAST_LOG			5	/* Native-TWAI fast datalogger (Task #18) */
#define POLL_LOG			6	/* Native-TWAI request/response poller (Task #18, Phase B) */

typedef enum
{
	WIFI_OPEN,
	WIFI_WPA2_PSK,
	WIFI_WPA3_PSK,
	WIFI_MAX
}wifi_security_t;

typedef enum
{
	LOG_SDCARD,
	LOG_INTERNAL,
	MAX_LOG_STORAGE
}log_storage_t;

typedef enum
{
	LOG_FS_LITTLEFS,
	LOG_FS_FATFS,
	MAX_LOG_FS
}log_filesystem_t;

typedef enum
{
	DRIVE_CONNECTION_WIFI,
	DRIVE_CONNECTION_BLE,
	DRIVE_CONNECTION_MAX
}drive_connection_type_t;

typedef struct _device_config
{
	char wifi_mode[65];
	char ap_ch[65];
	// Optional custom AP SSID
	// ap_ssid_en: "enable" or "disable" (default)
	char ap_ssid_en[10];
	char ap_ssid[65];
	char ap_auto_disable[12];
	char sta_ssid[65];
	char sta_pass[65];
	char sta_security[8];
	// Backup STA networks (fallbacks)
	// Parsed from JSON key: "sta_fallbacks" as array of objects
	//   { "ssid": string (1..32), "pass": string (0..64), "security": "open"|"wpa2"|"wpa3" }
	// Max 5 entries retained
	struct {
		char ssid[65];
		char pass[65];
		char security[8];
	} sta_fallbacks[5];
	int sta_fallbacks_count;
	char home_ssid[65];
	char home_password[65];
	char home_security[8];
	char home_protocol[65];
	char drive_ssid[65];
	char drive_password[65];
	char drive_security[8];
	char drive_protocol[65];
	char drive_connection_type[8];
	char drive_mode_timeout[8];
	char can_datarate[65];
	char can_mode[65];
	char ap_pass[65];
	char ble_pass[18];
	char ble_status[32];
	char ble_power[8]; // dBm value as string (e.g., -12, -9, -6, -3, 0, 3, 6, 9)
	char sleep_status[32];
	char can_wake[32];   /* #4 wake-on-CAN master switch */
	char sleep_disable_agree[10];
	char sleep_volt[10];
	char engine_volt[10];   // Task #6: dedicated engine-running gate for the CSV logger (separate from sleep_volt)
	char wakeup_volt[10];
	char sleep_time[32];
	char wakeup_time[32];
	char periodic_wakeup[32];
	char wakeup_interval[32];
	char batt_alert[32];
	char batt_alert_ssid[65];
	char batt_alert_pass[65];
	char batt_alert_volt[32];
	char batt_alert_protocol[65];
	char batt_alert_url[256];
	char batt_alert_port[32];
	char batt_alert_topic[256];
	char batt_alert_time[16];
	char batt_mqtt_user[64];
	char batt_mqtt_pass[64];
	// MQTT TLS verification behavior: "enable" to skip CN check, "disable" (default) to verify
	char csv_log[16];
	char log_storage[16];
	char log_filesystem[16];
	char log_period[16];
	char csv_grid_hz[16];     // wide fixed-rate grid frequency, 1..100 Hz or "auto" (issue #23)
	char csv_require_engine[16]; // gate CSV logging on engine running (ECU answering): "enable" | "disable"
	char imu_threshold[16];
	char led_blink[16];       // activity-LED blink toggle: "enable" (default) blinks while active, "disable" holds a solid color
	char timezone[64];        // POSIX TZ string for local wall-clock rendering, e.g. "PST8PDT,M3.2.0,M11.1.0" (issue #91)
	bool debug_enabled;
}device_config_t;


void config_server_start(QueueHandle_t *xRXp_Queue, uint8_t connected_led, char * did);
void config_server_stop(void);
int8_t config_server_get_wifi_mode(void);
int8_t config_server_get_ap_ch(void);
char *config_server_get_sta_ssid(void);
char *config_server_get_sta_pass(void);
char *config_server_get_home_ssid(void);
char *config_server_get_home_password(void);
char *config_server_get_home_security(void);
int8_t config_server_get_home_protocol(void);
char *config_server_get_drive_ssid(void);
char *config_server_get_drive_password(void);
char *config_server_get_drive_security(void);
int8_t config_server_get_drive_protocol(void);
drive_connection_type_t config_server_get_drive_connection_type(void);
char *config_server_get_drive_mode_timeout(void);
wifi_security_t config_server_get_home_security_type(void);
wifi_security_t config_server_get_drive_security_type(void);
int8_t config_server_get_can_rate(void);
int8_t config_server_get_can_mode(void);
//void config_server_wifi_connected(bool flag);
//bool config_server_get_wifi_connected(void);
void config_server_set_sta_ip(char* ip);
void config_server_get_sta_ip(char* ip);
char *config_server_get_ap_pass(void);
int8_t config_server_get_ap_ssid_en(void);
char *config_server_get_ap_ssid(void);
int8_t config_server_protocol(void);
/* Name of a resolved protocol enum value, for logging. Never NULL. */
const char *config_server_protocol_name(int8_t protocol);
int config_server_ble_pass(void);
int8_t config_server_get_sleep_config(void);
int8_t config_server_get_can_wake(void);

/* True while a firmware OTA upload is in flight. The sleep teardown refuses to sleep while this
 * is raised: sleeping calls wifi_mgr_deinit(), which tears the network stack out from under the
 * live upload and crashes the device. Distinct from the #86 ECU-flash interlock, which guards the
 * CAN bus and does not cover a WiFi firmware update. */
void config_server_ota_active_set(bool active);
bool config_server_ota_active(void);
int8_t config_server_get_ble_power(int8_t *power_dbm); // returns 0 on success
//void config_server_set_ble_tempfn(char b);
//char config_server_get_ble_tempfn(void);
int8_t config_server_get_ble_config(void);
void config_server_set_ble_config(uint8_t b);
void config_server_restart(void);
int8_t config_server_get_sleep_volt(float *sleep_volt);
int8_t config_server_get_engine_volt(float *engine_volt);   // Task #6: engine-running gate (13.0-15.0 V)
int8_t config_server_get_battery_alert_config(void);
int32_t config_server_get_alert_port(void);
char *config_server_get_alert_ssid(void);
char *config_server_get_alert_pass(void);
char *config_server_get_alert_protocol(void);
char *config_server_get_alert_url(void);
char *config_server_get_alert_topic(void);
int8_t config_server_get_alert_volt(float *alert_volt);
int config_server_get_alert_time(void);
char *config_server_get_alert_mqtt_user(void);
char *config_server_get_alert_mqtt_pass(void);
int8_t config_server_get_wakeup_volt(float *wakeup_volt);
int8_t config_server_get_sleep_time(uint32_t *sleep_time);
int8_t config_server_get_wakeup_time(uint32_t *wakeup_time);
wifi_security_t config_server_get_sta_security(void);
int8_t config_server_get_csv_log(void);
// Activity-LED blink toggle: 1 = blink while active (default), 0 = solid color.
int8_t config_server_get_led_blink_enabled(void);
int8_t config_server_get_log_period(uint32_t *log_period);
// Wide CSV (Task #11; always fixed-rate since issue #53): grid_hz writes *hz (1..100,
// or 0 for the "auto" sentinel -- issue #23: grid tracks the measured poll sweep rate) and
// returns 1, or -1 (leaves *hz untouched) on a bad value. Callers MUST handle *hz==0.
int8_t config_server_get_csv_grid_hz(uint32_t *hz);
// Engine-running CSV gate (default ON): 1=enable, 0=disable.
int8_t config_server_get_csv_require_engine(void);
log_storage_t config_server_get_log_storage(void);
log_filesystem_t config_server_get_log_filesystem(void);
int8_t config_server_get_ap_auto_disable(void);
int8_t config_server_get_periodic_wakeup(void);
int8_t config_server_get_wakeup_interval(uint32_t *wakeup_interval);
int8_t config_server_get_imu_threshold(uint8_t *imu_threshold);
bool config_server_is_debug_enabled(void);
// Fallback STA networks accessors
int config_server_get_sta_fallbacks_count(void);
const char *config_server_get_sta_fallback_ssid(int index);
const char *config_server_get_sta_fallback_pass(int index);
wifi_security_t config_server_get_sta_fallback_security(int index);
char *config_server_get_status_json(bool remove_sensitive_info);
