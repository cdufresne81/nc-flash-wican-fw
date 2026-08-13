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



#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include <nvs_flash.h>
#include <sys/param.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_tls_crypto.h"
#include <esp_http_server.h>
#include "freertos/timers.h"
#include "esp_err.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "ff.h"
#include "filesystem.h"
#if USE_FATFS	
#include "esp_vfs_fat.h"
#endif	

#include "esp_vfs.h"
#include "config_server.h"
#include "cJSON.h"
#include<stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include "ver.h"

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "lwip/sockets.h"

#include "esp_http_server.h"
#include "comm_server.h"
#include "types.h"
#include "driver/gpio.h"
#include "wifi_network.h"
#include "esp_vfs.h"
#include "esp_ota_ops.h"
#include "multipart_upload.h"
#include "can.h"
#include "ble.h"
#include "sleep_mode.h"
#include "vehicle.h"   // VEHICLE_ENGINE_ON_VOLT_DEFAULT_STR: engine_volt default, single-sourced
#include "autopid.h"
#include "wc_mdns.h"
#include "elm327.h"
#include "hw_config.h"
#include "rtcm.h"
#include "esp_littlefs.h"
#include "csv_logger.h"
#include "poll_log.h"
#include "event_log.h"
#include "esp_timer.h"   /* /wake_probe uptime reporting */
#include "sd_filemgr.h"
#include "sdcard.h"
#include "obd2_standard_pids.h"
#include "wifi_mgr.h"
#include "dev_status.h"
#include "dev_status.h"
#include "esp_heap_caps.h"
#include "autopid_http.h"
#include "obd2_standard_pids.h"
#include "restart_tracker.h"
#include "restart_tracker_http.h"
#include "led_indicator.h"


#define WIFI_CONNECTED_BIT			BIT0
static EventGroupHandle_t xServerEventGroup = NULL;
static StaticEventGroup_t server_event_group_buffer;
static QueueHandle_t xip_Queue = NULL;
static StaticQueue_t xip_queue_struct;
static uint8_t xip_queue_storage[20];

static uint8_t ws_led;
#define TAG "CONFIG_SERVER"

static restart_tracker_planned_reason_t s_reboot_reason = RESTART_TRACKER_PLANNED_REASON_USER_REQUEST;
static restart_tracker_source_t s_reboot_source = RESTART_TRACKER_SOURCE_WEB_UI;
static uint32_t s_reboot_flags = RESTART_TRACKER_FLAG_NONE;

httpd_handle_t server = NULL;
char *device_config_file = NULL;
static char *device_id;

//Function prototypes
static esp_err_t wifi_scan_handler(httpd_req_t *req);

extern const unsigned char homepage_start[] asm("_binary_homepage_html_start");
extern const unsigned char homepage_end[]   asm("_binary_homepage_html_end");
extern const unsigned char logo_start[] asm("_binary_logo_txt_start");
extern const unsigned char logo_end[]   asm("_binary_logo_txt_end");
extern const unsigned char chart_js_start[] asm("_binary_chart_js_start");
extern const unsigned char chart_js_end[] asm("_binary_chart_js_end");
extern const unsigned char sql_wasm_js_start[] asm("_binary_sqlwasm_js_start");
extern const unsigned char sql_wasm_js_end[] asm("_binary_sqlwasm_js_end");
extern const unsigned char sql_wasm_wasm_start[] asm("_binary_sqlwasm_wasm_start");
extern const unsigned char sql_wasm_wasm_end[] asm("_binary_sqlwasm_wasm_end");
extern const unsigned char bootstrap_bundle_min_js_start[] asm("_binary_bootstrap_bundle_min_js_start");
extern const unsigned char bootstrap_bundle_min_js_end[] asm("_binary_bootstrap_bundle_min_js_end");
extern const unsigned char daterangepicker_min_js_start[] asm("_binary_daterangepicker_min_js_start");
extern const unsigned char daterangepicker_min_js_end[] asm("_binary_daterangepicker_min_js_end");
extern const unsigned char jquery_min_js_start[] asm("_binary_jquery_min_js_start");
extern const unsigned char jquery_min_js_end[] asm("_binary_jquery_min_js_end");
extern const unsigned char moment_min_js_start[] asm("_binary_moment_min_js_start");
extern const unsigned char moment_min_js_end[] asm("_binary_moment_min_js_end");
extern const unsigned char chartjs_adapter_moment_min_js_start[] asm("_binary_chartjs_adapter_moment_min_js_start");
extern const unsigned char chartjs_adapter_moment_min_js_end[] asm("_binary_chartjs_adapter_moment_min_js_end");
extern const unsigned char jszip_min_js_start[] asm("_binary_jszip_min_js_start");
extern const unsigned char jszip_min_js_end[] asm("_binary_jszip_min_js_end");
extern const unsigned char daterangepicker_css_start[] asm("_binary_daterangepicker_css_start");
extern const unsigned char daterangepicker_css_end[] asm("_binary_daterangepicker_css_end");
extern const unsigned char bootstrap_min_css_start[] asm("_binary_bootstrap_min_css_start");
extern const unsigned char bootstrap_min_css_end[] asm("_binary_bootstrap_min_css_end");
extern const unsigned char lucide_icons_js_start[] asm("_binary_lucide_icons_js_start");
extern const unsigned char lucide_icons_js_end[] asm("_binary_lucide_icons_js_end");
extern const unsigned char main_js_start[] asm("_binary_main_js_start");
extern const unsigned char main_js_end[] asm("_binary_main_js_end");

typedef struct {
    const char *uri;
    const char *content_type;
    const unsigned char *data_start;
    const unsigned char *data_end;
    bool load_from_fs;      
    const char *fs_path;
} file_lookup_t;

static const file_lookup_t file_lookup[] = {
	{"/lucide_icons.js", "application/javascript", lucide_icons_js_start, lucide_icons_js_end, false, NULL},
	{"/main.js", "application/javascript", main_js_start, main_js_end, false, NULL},
	
	{NULL, NULL, NULL, NULL, false, NULL} // Sentinel to mark end of array
};

typedef struct {
    const char *uri;
    esp_err_t (*handler)(httpd_req_t *req);
} handler_lookup_t;

static const handler_lookup_t get_req_handler_lookup[] = {
	{"/wifi_scan", wifi_scan_handler},
	{NULL, NULL} 
};

static char can_datarate_str[11][7] = {
								"5k",
								"10K",
								"20K",
								"25K",
								"50K",
								"100K",
								"125K",
								"250K",
								"500K",
								"800K",
								"1000K",
};

const char device_config_default[] = "{\"wifi_mode\":\"AP\",\"ap_ch\":\"6\",\"sta_ssid\":\"MeatPi\",\"sta_pass\":\"TomatoSauce\",\"sta_security\":\"wpa3\",\
									\"ap_ssid_en\":\"disable\",\"ap_ssid\":\"\",\
										\"home_ssid\":\"MeatPi\",\"home_password\":\"TomatoSauce\",\"home_security\":\"wpa3\",\"home_protocol\":\"elm327\",\
										\"drive_ssid\":\"MeatPi\",\"drive_password\":\"TomatoSauce\",\"drive_security\":\"wpa3\",\"drive_protocol\":\"elm327\",\"drive_connection_type\":\"wifi\",\"drive_mode_timeout\":\"60\",\
										\"can_datarate\":\"500K\",\
										\"can_mode\":\"normal\",\"port_type\":\"tcp\",\"port\":\"35000\",\"ap_pass\":\"@meatpi#\",\"protocol\":\"poll_log\",\"ble_pass\":\"123456\",\
								\"ble_status\":\"disable\",\"ble_power\":\"9\",\"sleep_status\":\"enable\",\"can_wake\":\"enable\",\"periodic_wakeup\":\"disable\",\"sleep_volt\":\"13.0\",\"engine_volt\":\"13.0\",\"wakeup_volt\":\"13.5\",\"sleep_time\":\"5\",\"wakeup_interval\":\"90\",\"batt_alert\":\"disable\",\
										\"batt_alert_ssid\":\"MeatPi\",\"batt_alert_pass\":\"TomatoSauce\",\"batt_alert_volt\":\"11.0\",\"batt_alert_protocol\":\"mqtt\",\
										\"batt_alert_url\":\"mqtt://mqtt.eclipseprojects.io\",\"batt_alert_port\":\"1883\",\"batt_alert_topic\":\"CAR1/voltage\",\"batt_mqtt_user\":\"meatpi\",\
								\"batt_mqtt_pass\":\"meatpi\",\"batt_alert_time\":\"1\",\
										\"csv_log\":\"disable\",\"log_filesystem\":\"littlefs\",\"log_storage\":\"sdcard\",\"log_period\":\"10\",\"csv_grid_hz\":\"auto\",\"csv_require_engine\":\"enable\",\"led_blink\":\"enable\"}";

// const char device_config_default[] = "{\"wifi_mode\":\"AP\",\"ap_ch\":\"6\", \"ap_auto_disable\": \"disable\",\"sta_ssid\":\"MeatPi\",\"sta_pass\":\"TomatoSauce\",\"sta_security\":\"wpa3\",\"can_datarate\":\"500K\",\"can_mode\":\"normal\",\"port_type\":\"tcp\",\"port\":\"35000\",\"ap_pass\":\"@meatpi#\",\"protocol\":\"elm327\",\"ble_pass\":\"123456\",\"ble_status\":\"disable\",\"sleep_status\":\"disable\",\"sleep_volt\":\"13.1\",\"wakeup_volt\":\"13.5\",\"batt_alert\":\"disable\",\"batt_alert_ssid\":\"MeatPi\",\"batt_alert_pass\":\"TomatoSauce\",\"batt_alert_volt\":\"11.0\",\"batt_alert_protocol\":\"mqtt\",\"batt_alert_url\":\"mqtt://mqtt.eclipseprojects.io\",\"batt_alert_port\":\"1883\",\"batt_alert_topic\":\"CAR1/voltage\",\"batt_mqtt_user\":\"meatpi\",\"batt_mqtt_pass\":\"meatpi\",\"batt_alert_time\":\"1\",\"mqtt_user\":\"meatpi\",\"mqtt_pass\":\"meatpi\",\"mqtt_tx_topic\":\"wican/%s/can/tx\",\"mqtt_rx_topic\":\"wican/%s/can/rx\",\"mqtt_status_topic\":\"wican/%s/can/status\"}";
// const char device_config_default[] = "{\"wifi_mode\":\"AP\",\"ap_ch\":\"6\", \"ap_auto_disable\": \"disable\",\"sta_ssid\":\"MeatPi\",\"sta_pass\":\"TomatoSauce\",\"sta_security\":\"wpa3\",\"can_datarate\":\"500K\",\"can_mode\":\"normal\",\"port_type\":\"tcp\",\"port\":\"35000\",\"ap_pass\":\"@meatpi#\",\"protocol\":\"elm327\",\"ble_pass\":\"123456\",\"ble_status\":\"disable\",\"sleep_status\":\"disable\",\"sleep_volt\":\"13.1\",\"wakeup_volt\":\"13.5\",\"periodic_wakeup\":\"disable\",\"wakeup_interval\":\"5\",\"batt_alert\":\"disable\",\"batt_alert_ssid\":\"MeatPi\",\"batt_alert_pass\":\"TomatoSauce\",\"batt_alert_volt\":\"11.0\",\"batt_alert_protocol\":\"mqtt\",\"batt_alert_url\":\"mqtt://mqtt.eclipseprojects.io\",\"batt_alert_port\":\"1883\",\"batt_alert_topic\":\"CAR1/voltage\",\"batt_mqtt_user\":\"meatpi\",\"batt_mqtt_pass\":\"meatpi\",\"batt_alert_time\":\"1\",\"mqtt_user\":\"meatpi\",\"mqtt_pass\":\"meatpi\",\"mqtt_tx_topic\":\"wican/%s/can/tx\",\"mqtt_rx_topic\":\"wican/%s/can/rx\",\"mqtt_status_topic\":\"wican/%s/can/status\"}";
static device_config_t device_config;
TimerHandle_t xrestartTimer;

// Boot parser hoisted out of config_server_load_cfg so the live-apply path in
// store_config_handler can parse into a scratch struct without side effects.
// Returns false on any parse/validation error (caller decides recovery).
static bool config_server_parse_cfg_into(device_config_t *dst, const char *cfg);

static void config_server_schedule_reboot(restart_tracker_planned_reason_t reason,
								  restart_tracker_source_t source,
								  uint32_t flags)
{
	s_reboot_reason = reason;
	s_reboot_source = source;
	s_reboot_flags = flags;
	xTimerStart(xrestartTimer, 0);
}

void config_server_reboot(void)
{
	gpio_set_level(CAN_STDBY_GPIO_NUM, 1);
	elm327_sleep();
	ESP_LOGI(TAG, "reboot");
	printf("reboot command\n");
	config_server_schedule_reboot(RESTART_TRACKER_PLANNED_REASON_USER_REQUEST,
						 RESTART_TRACKER_SOURCE_WEB_UI,
						 RESTART_TRACKER_FLAG_NONE);
}

/* Max length a file path can have on storage */
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)
/* Scratch buffer size */
#define SCRATCH_BUFSIZE  (1024*64)

#define MAX_FILE_SIZE   (2000*1024) // 2000 KB
#define MAX_FILE_SIZE_STR "2000KB"

#define AP_SSID_MIN_LEN 3
#define AP_SSID_MAX_LEN 32

struct file_server_data {
    /* Base path of file storage */
    char base_path[ESP_VFS_PATH_MAX + 1];

    /* Scratch buffer for temporary storage during file transfer */
    char *scratch;
};


/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        /* Full path string won't fit into destination buffer */
        return NULL;
    }

    /* Construct full path (base + path) */
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);

    /* Return pointer to path, skipping the base */
    return dest + base_pathlen;
}

int8_t config_server_get_wifi_mode(void)
{
	if(strcmp(device_config.wifi_mode, "AP") == 0)
	{
		return AP_MODE;
	}
	else if(strcmp(device_config.wifi_mode, "APStation") == 0)
	{
		return APSTA_MODE;
	}
	else if(strcmp(device_config.wifi_mode, "BLEStation") == 0)
	{
		return BLESTA_MODE;
	}
	else if(strcmp(device_config.wifi_mode, "Station") == 0)
	{
		return STA_MODE;
	}
	else if(strcmp(device_config.wifi_mode, "SmartConnect") == 0)
	{
		return SMARTCONNECT_MODE;
	}
	return -1;
}

int8_t config_server_get_ap_ch(void)
{
	int ch_val = atoi(device_config.ap_ch);

	if(ch_val > 0 && ch_val < 15)
	{
		return ch_val;
	}
	return -1;
}

char *config_server_get_sta_ssid(void)
{
	return device_config.sta_ssid;
}

char *config_server_get_ap_pass(void)
{
	return device_config.ap_pass;
}

int8_t config_server_get_ap_ssid_en(void)
{
	return (strcmp(device_config.ap_ssid_en, "enable") == 0) ? 1 : 0;
}

char *config_server_get_ap_ssid(void)
{
	return device_config.ap_ssid;
}

int config_server_ble_pass(void)
{
	int ble_pass = atoi(device_config.ble_pass);

	if(ble_pass > 0 && ble_pass <= 999999)
	{
		return ble_pass;
	}
	return -1;
}

char *config_server_get_sta_pass(void)
{
	return device_config.sta_pass;
}

char *config_server_get_home_ssid(void)
{
	return device_config.home_ssid;
}

char *config_server_get_home_password(void)
{
	return device_config.home_password;
}

char *config_server_get_home_security(void)
{
	return device_config.home_security;
}

int8_t config_server_get_home_protocol(void)
{
	if(strcmp(device_config.home_protocol, "slcan") == 0)
	{
		return SLCAN;
	}
	else if(strcmp(device_config.home_protocol, "elm327") == 0)
	{
		return OBD_ELM327;
	}
	// No auto_pid arm: the legacy AutoPID scheduler is retired on this fork (issue #28), so a
	// stored "auto_pid" falls through to the ELM327 default below. This is the single place that
	// decision lives -- main.c forces protocol = AUTO_PID from this getter under SmartConnect, so
	// resurrecting the arm here would restart the legacy poller.
	return OBD_ELM327;
}

char *config_server_get_drive_ssid(void)
{
	return device_config.drive_ssid;
}

char *config_server_get_drive_password(void)
{
	return device_config.drive_password;
}

char *config_server_get_drive_security(void)
{
	return device_config.drive_security;
}

int8_t config_server_get_drive_protocol(void)
{
	if(strcmp(device_config.drive_protocol, "slcan") == 0)
	{
		return SLCAN;
	}
	else if(strcmp(device_config.drive_protocol, "elm327") == 0)
	{
		return OBD_ELM327;
	}
	// See config_server_get_home_protocol(): no auto_pid arm, by design.
	return OBD_ELM327;
}

drive_connection_type_t config_server_get_drive_connection_type(void)
{
	if(strcmp(device_config.drive_connection_type, "wifi") == 0)
	{
		return DRIVE_CONNECTION_WIFI;
	}
	else if(strcmp(device_config.drive_connection_type, "ble") == 0)
	{
		return DRIVE_CONNECTION_BLE;
	}
	return DRIVE_CONNECTION_WIFI; // Default to WiFi
}

char *config_server_get_drive_mode_timeout(void)
{
	return device_config.drive_mode_timeout;
}

wifi_security_t config_server_get_home_security_type(void)
{
	if(strcmp(device_config.home_security, "wpa2") == 0)
	{
		return WIFI_WPA2_PSK;
	}
	else if(strcmp(device_config.home_security, "wpa3") == 0)
	{
		return WIFI_WPA3_PSK;
	}
	return WIFI_WPA3_PSK; // Default to WPA3
}

wifi_security_t config_server_get_drive_security_type(void)
{
	if(strcmp(device_config.drive_security, "wpa2") == 0)
	{
		return WIFI_WPA2_PSK;
	}
	else if(strcmp(device_config.drive_security, "wpa3") == 0)
	{
		return WIFI_WPA3_PSK;
	}
	return WIFI_WPA3_PSK; // Default to WPA3
}

int8_t config_server_protocol(void)
{
	if(strcmp(device_config.protocol, "slcan") == 0)
	{
		return SLCAN;
	}
	else if(strcmp(device_config.protocol, "elm327") == 0)
	{
		return OBD_ELM327;
	}
	// No auto_pid arm: the parser coerces a stored "auto_pid" to "poll_log" before it can reach
	// device_config, so this string is unreachable. Kept out rather than left dead so nobody
	// reads it as evidence that AUTO_PID is still a selectable protocol.
	else if(strcmp(device_config.protocol, "fast_log") == 0)
	{
		return FAST_LOG;
	}
	else if(strcmp(device_config.protocol, "poll_log") == 0)
	{
		return POLL_LOG;
	}
	return OBD_ELM327;
}

int8_t config_server_get_can_rate(void)
{
	ESP_LOGI(TAG, "device_config.can_datarate:%s", device_config.can_datarate);
	if(strcmp(device_config.can_datarate, "5K") == 0)
	{
		return CAN_5K;
	}
	if(strcmp(device_config.can_datarate, "10K") == 0)
	{
		return CAN_10K;
	}
	if(strcmp(device_config.can_datarate, "20K") == 0)
	{
		return CAN_20K;
	}
	if(strcmp(device_config.can_datarate, "25K") == 0)
	{
		return CAN_25K;
	}
	else if(strcmp(device_config.can_datarate, "50K") == 0)
	{
		return CAN_50K;
	}
	else if(strcmp(device_config.can_datarate, "100K") == 0)
	{
		return CAN_100K;
	}
	else if(strcmp(device_config.can_datarate, "125K") == 0)
	{
		return CAN_125K;
	}
	else if(strcmp(device_config.can_datarate, "250K") == 0)
	{
		return CAN_250K;
	}
	else if(strcmp(device_config.can_datarate, "500K") == 0)
	{
		return CAN_500K;
	}
	else if(strcmp(device_config.can_datarate, "800K") == 0)
	{
		return CAN_800K;
	}
	else if(strcmp(device_config.can_datarate, "1000K") == 0)
	{
		return CAN_1000K;
	}
	else if(strcmp(device_config.can_datarate, "auto") == 0)
	{
		return CAN_AUTO;
	}

	return -1;
}


int8_t config_server_get_can_mode(void)
{
	if(strcmp(device_config.can_mode, "normal") == 0)
	{
		return CAN_NORMAL;
	}
	else if(strcmp(device_config.can_mode, "silent") == 0)
	{
		return CAN_SILENT;
	}
	return -1;
}

int8_t config_server_get_port_type(void)
{
	if(strcmp(device_config.port_type, "tcp") == 0)
	{
		return TCP_PORT;
	}
	else if(strcmp(device_config.port_type, "udp") == 0)
	{
		return UDP_PORT;
	}
	return -1;
}

int32_t config_server_get_port(void)
{
	int port_val = atoi(device_config.port);

	if(port_val > 0 && port_val <= 65535)
	{
		return port_val;
	}

	ESP_LOGE(TAG, "Invalid port number in config");
	return 35000;
}

// Create directories recursively if they don't exist
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    if(config_server_get_ble_config() == 1){
        ESP_LOGW(TAG, "BLE is enabled, disable BLE to perform WiFi scan");
        const char *ble_error_response = "{\"error\":\"Go to Settings -> BLE, disable it and Submit Changes to perform WiFi scan\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, ble_error_response, strlen(ble_error_response));
        return ESP_OK;
    }

    char *scan_results = wifi_mgr_scan_networks();

    if (scan_results != NULL) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, scan_results, strlen(scan_results));
        free(scan_results);
    } else {
        ESP_LOGE(TAG, "WiFi scan failed");
        const char *scan_error_response = "{\"error\":\"WiFi scan failed\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, scan_error_response, strlen(scan_error_response));
    }
    return ESP_OK;
}

static esp_err_t get_uri_handler(httpd_req_t *req)
{
	const char *uri = req->uri; // make const to avoid discarding qualifier
	const uint32_t chunk_size = 1024 * 64;
    ESP_LOGI(TAG, "Request URI: %s", uri);
    
    const handler_lookup_t *handler = get_req_handler_lookup;
	
    while (handler->uri != NULL) {
        if (strcmp(uri, handler->uri) == 0) {
			handler->handler(req);
            return ESP_OK;
        }
        handler++;
    }

    const file_lookup_t *file = file_lookup;
    while (file->uri != NULL) {
        if (strcmp(uri, file->uri) == 0) {
            ESP_LOGI(TAG, "Found file entry: %s", file->uri);
            
            // If configured to load from filesystem
            if (file->load_from_fs && file->fs_path != NULL) {
                struct stat st;
                if (stat(file->fs_path, &st) == 0) {
                    // File exists on filesystem, serve it
                    ESP_LOGI(TAG, "Serving file from filesystem: %s", file->fs_path);
                    FILE *fp = fopen(file->fs_path, "r");
                    if (fp) {
                        httpd_resp_set_type(req, file->content_type);
                        char *buffer = malloc(chunk_size);
                        if (!buffer) {
                            fclose(fp);
                            return ESP_ERR_NO_MEM;
                        }
                        memset(buffer, 0, chunk_size);

                        size_t bytes_read;
                        while ((bytes_read = fread(buffer, 1, chunk_size, fp)) > 0) {
                            httpd_resp_send_chunk(req, buffer, bytes_read);
                        }
                        httpd_resp_send_chunk(req, NULL, 0); // End response
                        free(buffer);
                        fclose(fp);
                        return ESP_OK;
                    }
                }
            }
            
            // Fallback to serving from memory if filesystem loading failed or wasn't configured
            if (file->data_start != NULL && file->data_end != NULL) {
                ESP_LOGI(TAG, "Serving file from memory: %s", file->uri);
                httpd_resp_set_type(req, file->content_type);
                const size_t file_size = file->data_end - file->data_start;
                esp_err_t ret = httpd_resp_send(req, (const char*)file->data_start, file_size);
                return (ret == ESP_OK) ? ESP_OK : ESP_FAIL;
            }
            
            // If we get here, we couldn't serve the file
            ESP_LOGE(TAG, "Failed to serve file: %s", file->uri);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        file++;
    }
    
    // If we get here, the requested URI wasn't found in our lookup table
    // Return 404 Not Found
    httpd_resp_send_404(req);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    
    const size_t homepage_size = homepage_end - homepage_start;
    
    esp_err_t ret = httpd_resp_send(req, (const char*)homepage_start, homepage_size);
    
    return (ret == ESP_OK) ? ESP_OK : ESP_FAIL;
}

// LIVE whitelist (issue #39): the config.json keys that apply at runtime without a
// reboot because every consumer re-reads them live (LED task each loop, smartconnect
// per transition, batt-alert creds/protocol when an alert fires). Any change to a
// field NOT listed here still forces a reboot via the memcmp(probe,shadow) backstop.
// batt_alert (master) and periodic_wakeup are inert: the parser force-disables both, so
// neither can ever diff.
// Deliberately EXCLUDED (stay reboot-required): batt_alert_volt, batt_alert_time
// (cached once into adc_task statics). Keep this list in lockstep with
// docs/goal-live-reconfigure.md (exactly 20 keys).
#define LIVE_APPLY_WHITELIST(X) \
	X(led_blink) \
	X(home_ssid) X(home_password) X(home_security) X(home_protocol) \
	X(drive_ssid) X(drive_password) X(drive_security) X(drive_protocol) \
	X(drive_connection_type) X(drive_mode_timeout) \
	X(batt_alert) X(batt_alert_protocol) X(batt_alert_ssid) X(batt_alert_pass) \
	X(batt_alert_url) X(batt_alert_port) X(batt_alert_topic) \
	X(batt_mqtt_user) X(batt_mqtt_pass)

// ---- Stored secrets ---------------------------------------------------------
// Every credential the device stores, in one place: the JSON key on the wire and
// the matching device_config_t field. This drives all three halves of the
// contract -- redaction on the way out (/check_status, /load_config), restoration
// on the way in (/store_config), and log redaction in the parser -- so a secret
// added later cannot be handled in one place and forgotten in the others.
#define CONFIG_SECRET_FIELDS(X) \
	X("sta_pass",        sta_pass) \
	X("ap_pass",         ap_pass) \
	X("ble_pass",        ble_pass) \
	X("home_password",   home_password) \
	X("drive_password",  drive_password) \
	X("batt_alert_pass", batt_alert_pass) \
	X("batt_mqtt_pass",  batt_mqtt_pass)

// Sent in place of a stored secret, so the UI can show that one is set and hand
// it back untouched on the next Submit without ever holding the real value.
//
// Redaction and round-tripping have to be solved together: the UI repopulates
// its form from the device and posts the whole form back, so redacting to an
// empty string would erase the user's Wi-Fi password on their next Submit. That
// coupling is why the redaction plumbing already in this file was never switched
// on. A placeholder breaks the tie.
//
// The 0x01 bytes are control characters. A browser will not put them in a
// password field, so a real password cannot collide with this sentinel; if one
// somehow did, the failure is benign -- the stored secret simply stays as it was.
#define CONFIG_SECRET_PLACEHOLDER "\x01stored\x01"

static inline bool config_secret_is_placeholder(const char *v)
{
	return v != NULL && strcmp(v, CONFIG_SECRET_PLACEHOLDER) == 0;
}

// Log a credential's shape, never its value. The length is what makes these
// lines useful for debugging (it distinguishes "never set" from "set but
// truncated"), and it is not the secret. These fire on every /store_config, not
// just at boot, and the log ring is readable over the network via /event_log.
#define CONFIG_LOG_SECRET(NAME, VAL) \
	ESP_LOGI(TAG, NAME ": %s (%u chars)", (VAL)[0] ? "<set>" : "<empty>", \
		 (unsigned)strlen(VAL))

// Emit a secret into a response: the real value when not redacting, the
// placeholder when redacting and something is stored, and an empty string when
// nothing is stored -- so "no password set" stays distinguishable from "hidden".
static void config_add_secret(cJSON *root, const char *key, const char *value,
			      bool redact)
{
	if (redact)
	{
		cJSON_AddStringToObject(root, key,
			(value && value[0]) ? CONFIG_SECRET_PLACEHOLDER : "");
		return;
	}
	cJSON_AddStringToObject(root, key, value ? value : "");
}

// Replace any placeholder the UI handed back with the value already stored, so
// the caller can persist the result verbatim. Returns the number restored.
// Anything that is not the exact placeholder is left alone and taken literally,
// which keeps a direct API client (curl) working as before.
static int config_restore_placeholder_secrets(cJSON *root)
{
	int restored = 0;
	#define RESTORE_SECRET(KEY, FIELD)                                           \
		{                                                                    \
			cJSON *item = cJSON_GetObjectItem(root, KEY);                \
			if (item && cJSON_IsString(item) &&                          \
			    config_secret_is_placeholder(item->valuestring))         \
			{                                                            \
				cJSON_SetValuestring(item, device_config.FIELD);      \
				restored++;                                          \
			}                                                            \
		}
	CONFIG_SECRET_FIELDS(RESTORE_SECRET)
	#undef RESTORE_SECRET
	return restored;
}

// Single authority for the honest apply-envelope wire contract (issue #39), shared by
// /store_config and /store_auto_data. `applied` is one of "reboot"|"live"|"deferred";
// `msg` is a fixed literal (never user input), so no JSON escaping is needed.
static esp_err_t config_server_send_apply_envelope(httpd_req_t *req, bool reboot,
						   const char *applied, const char *msg)
{
	char body[256];
	snprintf(body, sizeof body,
		 "{\"reboot\":%s,\"applied\":\"%s\",\"msg\":\"%s\"}",
		 reboot ? "true" : "false", applied, msg);
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t store_config_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "store_config_handler called: content_len=%d", req ? req->content_len : -1);

	esp_err_t ret_val = ESP_OK;
	FILE *f = NULL;

	if (req == NULL)
	{
		return ESP_ERR_INVALID_ARG;
	}

	// Validate content type
	char content_type[32];
	if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK)
	{
		if (strncmp(content_type, "application/json", 16) != 0)
		{
			ESP_LOGE(TAG, "Invalid content type: %s", content_type);
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content type must be application/json");
			return ESP_FAIL;
		}
	}

	// Check content length
	int total_len = req->content_len;
	if (total_len <= 0 || total_len > MAX_FILE_SIZE)
	{
		ESP_LOGE(TAG, "Invalid content length: %d (max %s)", total_len, MAX_FILE_SIZE_STR);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
		return ESP_FAIL;
	}

	// Allocate memory in PSRAM for the full payload (+1 for NUL)
	char *buf = (char *)heap_caps_malloc(total_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (buf == NULL)
	{
		ESP_LOGE(TAG, "Failed to allocate %d bytes in PSRAM for config payload", total_len + 1);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
		return ESP_ERR_NO_MEM;
	}
	memset(buf, 0, total_len + 1);

	// Receive the data in chunks
	int received = 0;
	while (received < total_len)
	{
		int to_read = MIN(4096, total_len - received);
		int r = httpd_req_recv(req, buf + received, to_read);

		if (r < 0)
		{
			if (r == HTTPD_SOCK_ERR_TIMEOUT)
			{
				continue; // retry on timeout
			}
			ESP_LOGE(TAG, "Receive error: %d after %d/%d bytes", r, received, total_len);
			ret_val = ESP_FAIL;
			break;
		}
		if (r == 0)
		{
			ESP_LOGE(TAG, "Connection closed unexpectedly after %d/%d bytes", received, total_len);
			ret_val = ESP_FAIL;
			break;
		}

		received += r;
		ESP_LOGD(TAG, "Received chunk: %d bytes (total %d/%d)", r, received, total_len);
	}

	if (ret_val != ESP_OK)
	{
		free(buf);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
		return ESP_FAIL;
	}

	// Null-terminate and validate JSON
	buf[received] = '\0';
	cJSON *json = cJSON_Parse(buf);
	if (!json)
	{
		const char *err_ptr = cJSON_GetErrorPtr();
		ESP_LOGE(TAG, "Invalid JSON format%s%s",
				 err_ptr ? " near: " : "",
				 err_ptr ? err_ptr : "");
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
		free(buf);
		return ESP_FAIL;
	}

	// Validate optional custom AP SSID (avoid accepting a config that will be rejected on reboot)
	{
		bool ap_ssid_enabled = false;
		cJSON *k = cJSON_GetObjectItem(json, "ap_ssid_en");
		if (k)
		{
			if (cJSON_IsString(k) && k->valuestring)
			{
				ap_ssid_enabled = (strcmp(k->valuestring, "enable") == 0);
			}
			else if (cJSON_IsBool(k))
			{
				ap_ssid_enabled = cJSON_IsTrue(k);
			}
		}
		if (ap_ssid_enabled)
		{
			cJSON *v = cJSON_GetObjectItem(json, "ap_ssid");
			const char *ssid = (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
			size_t len = ssid ? strlen(ssid) : 0;
			if (!ssid || len < AP_SSID_MIN_LEN || len > AP_SSID_MAX_LEN)
			{
				httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ap_ssid must be 3-32 characters when enabled");
				cJSON_Delete(json);
				free(buf);
				return ESP_FAIL;
			}
		}
	}

	// ---- Restore placeholder secrets ----------------------------------------
	// The settings form is repopulated from a redacted /load_config, so a password
	// the user did not touch comes back as CONFIG_SECRET_PLACEHOLDER. Swap the
	// stored value back in HERE, before anything downstream sees the body: the
	// shadow parse, the file write and the cached raw config all consume this
	// buffer, and persisting the placeholder would destroy the real password on
	// the next boot. Only exact placeholders are touched, so a direct API client
	// posting a real password is unaffected.
	if (config_restore_placeholder_secrets(json) > 0)
	{
		char *restored_body = cJSON_PrintUnformatted(json);
		if (restored_body == NULL)
		{
			ESP_LOGE(TAG, "config: failed to serialize restored secrets");
			cJSON_Delete(json);
			free(buf);
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
			return ESP_ERR_NO_MEM;
		}
		size_t restored_len = strlen(restored_body);
		// Restoring makes the body longer (a real password outweighs the
		// placeholder), so re-check it against the same ceiling as the raw upload.
		if (restored_len > MAX_FILE_SIZE)
		{
			ESP_LOGE(TAG, "config: payload %u bytes after restoring secrets exceeds max %s",
				 (unsigned)restored_len, MAX_FILE_SIZE_STR);
			cJSON_free(restored_body);
			cJSON_Delete(json);
			free(buf);
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Configuration too large");
			return ESP_FAIL;
		}
		char *swap = (char *)heap_caps_malloc(restored_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (swap == NULL)
		{
			ESP_LOGE(TAG, "config: failed to allocate %u bytes for restored payload",
				 (unsigned)(restored_len + 1));
			cJSON_free(restored_body);
			cJSON_Delete(json);
			free(buf);
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
			return ESP_ERR_NO_MEM;
		}
		memcpy(swap, restored_body, restored_len + 1);
		cJSON_free(restored_body);
		free(buf);
		buf = swap;
		received = (int)restored_len;
	}

	cJSON_Delete(json);

	// ---- Validate required keys BEFORE persisting (issue #44) ----------------
	// Parse the payload with the real boot parser first, into a shadow seeded from
	// the live config. If a required key is missing/invalid the boot parse would
	// fail too and factory-restore every setting -- so reject here (HTTP 400) and
	// never touch config.json. The shadow is reused for the live-apply decision
	// below, so config.json is parsed only once.
	bool do_reboot = true;   // default to the always-correct behavior
	device_config_t *shadow = heap_caps_malloc(sizeof *shadow, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (shadow == NULL)
	{
		ESP_LOGE(TAG, "config validate: allocation failed, not persisting");
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
		free(buf);
		return ESP_ERR_NO_MEM;
	}
	// Seed shadow from the LIVE config (padding/skipped fields become byte-equal to
	// RAM so the whitelist diff can't false-positive), then parse into it.
	memcpy(shadow, &device_config, sizeof *shadow);
	if (!config_server_parse_cfg_into(shadow, buf))
	{
		ESP_LOGE(TAG, "config rejected: missing/invalid required field, not persisting");
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
			"Configuration rejected: a required field is missing or invalid");
		free(shadow);
		free(buf);
		return ESP_FAIL;
	}

	// Config parses cleanly -> safe to persist (won't factory-restore on reboot).
	f = fopen(FS_MOUNT_POINT "/config.json", "w");
	if (!f)
	{
		ESP_LOGE(TAG, "Failed to open %s for writing", FS_MOUNT_POINT "/config.json");
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save configuration");
		free(shadow);
		free(buf);
		return ESP_FAIL;
	}
	size_t written = fwrite(buf, 1, (size_t)received, f);
	if (written != (size_t)received)
	{
		ESP_LOGE(TAG, "Failed to write configuration: %zu/%d bytes written", written, received);
		fclose(f);
		free(shadow);
		free(buf);
		return ESP_FAIL;
	}
	fclose(f);

	// ---- Live-apply decision (issue #39) -------------------------------------
	// Reuse the shadow parsed above so a live apply is byte-for-byte reboot-
	// equivalent. Reboot dominates: apply live ONLY when every changed field is
	// whitelisted. probe is scratch for the whitelist diff and is needed only
	// here -- allocate it now; if that fails, fall back to the always-correct
	// reboot (the config is already validated and persisted).
	device_config_t *probe = heap_caps_malloc(sizeof *probe, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (probe != NULL)
	{
		// probe = live config with ONLY the whitelisted fields overwritten by the
		// parsed values (full-field memcpy, never strlcpy). If probe still differs
		// from the fully-parsed shadow, a non-whitelist field changed.
		memcpy(probe, &device_config, sizeof *probe);
		#define LIVE_APPLY_FIELD(F) memcpy((char *)probe  + offsetof(device_config_t, F), \
		                                   (char *)shadow + offsetof(device_config_t, F), \
		                                   sizeof shadow->F);
		LIVE_APPLY_WHITELIST(LIVE_APPLY_FIELD)
		#undef LIVE_APPLY_FIELD

		if (memcmp(probe, shadow, sizeof *shadow) == 0)
		{
			// Only whitelisted fields changed -> copy each into the live config
			// (field-width only; NEVER whole-struct memcpy, which would rewrite
			// reboot fields and transiently zero sta_fallbacks).
			#define LIVE_APPLY_FIELD(F) memcpy(&device_config.F, &shadow->F, sizeof device_config.F);
			LIVE_APPLY_WHITELIST(LIVE_APPLY_FIELD)
			#undef LIVE_APPLY_FIELD

			// Refresh the cached raw-config string so /load_config and the next
			// Submit don't revert the live change (httpd handlers serialize on one
			// task -> plain swap+free is safe here).
			char *fresh = heap_caps_malloc((size_t)received + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
			if (fresh != NULL)
			{
				memcpy(fresh, buf, (size_t)received);
				fresh[received] = '\0';
				char *old = device_config_file;
				device_config_file = fresh;
				free(old);
			}
			do_reboot = false;
		}
		free(probe);
	}
	free(shadow);

	// Honest envelope: reboot only when a reboot-required field changed.
	if (do_reboot)
	{
		config_server_send_apply_envelope(req, true, "reboot",
			"Configuration saved. Rebooting to apply.");
		config_server_schedule_reboot(RESTART_TRACKER_PLANNED_REASON_CONFIG_APPLY,
							 RESTART_TRACKER_SOURCE_WEB_UI,
							 RESTART_TRACKER_FLAG_SETTINGS_SAVED);
	}
	else
	{
		config_server_send_apply_envelope(req, false, "live",
			"Configuration applied (no reboot).");
		ESP_LOGI(TAG, "config applied live (no reboot)");
	}

	free(buf);
	return ESP_OK;
}



static esp_err_t load_pid_auto_handler(httpd_req_t *req)
{
    FILE *f = fopen(FS_MOUNT_POINT"/auto_pid.json", "r");
    if (f == NULL) 
	{
        const char* resp_str = "NONE";
        httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
        ESP_LOGI(TAG, "auto_pid.json: NONE");
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long filesize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(filesize + 1);
    if (!buf)
	{
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

	memset(buf, 0, filesize + 1);

    size_t read = fread(buf, 1, filesize, f);
    fclose(f);
    
    if (read != filesize)
	{
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    buf[filesize] = 0;
    ESP_LOGI(TAG, "auto_pid.json: %s", buf);
    
	httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);

    return ESP_OK;
}

static esp_err_t load_config_handler(httpd_req_t *req)
{
	// This is what seeds the settings form, so it must keep returning every key
	// (main.js Load() repopulates from it, and PASSTHROUGH_KEYS re-sends UI-less
	// keys verbatim). Serve a copy with the secrets swapped for the placeholder
	// rather than the raw stored file; /store_config swaps them back on the way in.
	cJSON *root = cJSON_Parse((const char *)device_config_file);
	if (root == NULL)
	{
		// Unparseable stored config: say so rather than falling back to sending
		// the raw file, which is exactly the leak this is here to prevent.
		ESP_LOGE(TAG, "load_config: stored config is not valid JSON");
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Stored configuration is unreadable");
		return ESP_FAIL;
	}

	#define REDACT_SECRET(KEY, FIELD)                                            \
		{                                                                    \
			cJSON *item = cJSON_GetObjectItem(root, KEY);                \
			if (item && cJSON_IsString(item) && item->valuestring)       \
			{                                                            \
				cJSON_SetValuestring(item, item->valuestring[0]       \
					? CONFIG_SECRET_PLACEHOLDER : "");           \
			}                                                            \
		}
	CONFIG_SECRET_FIELDS(REDACT_SECRET)
	#undef REDACT_SECRET

	char *resp_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (resp_str == NULL)
	{
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
		return ESP_FAIL;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
	cJSON_free(resp_str);
	return ESP_OK;
}


static esp_err_t system_reboot_handler(httpd_req_t *req)
{
	const char *resp_str = "Configuration saved! Rebooting...";
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
	config_server_schedule_reboot(RESTART_TRACKER_PLANNED_REASON_USER_REQUEST,
						 RESTART_TRACKER_SOURCE_WEB_UI,
						 RESTART_TRACKER_FLAG_NONE);
	// elm327_sleep();
	// ESP_LOGI(TAG, "reboot");
	// xTimerStart( xrestartTimer, 0 );
    // esp_restart();
    return ESP_OK;
}

static esp_err_t system_commands_handler(httpd_req_t *req)
{
    char *buf = NULL;
    size_t buf_size = req->content_len;

    if (buf_size <= 0)
    {
        return ESP_FAIL;
    }

    buf = (char *)malloc(buf_size + 1);
    if (!buf)
    {
        ESP_LOGE(TAG, "Memory allocation failure");
        return ESP_ERR_NO_MEM;
    }
	memset(buf, 0, buf_size + 1);

    int ret = httpd_req_recv(req, buf, buf_size);
    if (ret <= 0)
    {
        free(buf);
        return ESP_FAIL;
    }
    buf[ret] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL)
    {
        free(buf);
        return ESP_FAIL;
    }

    cJSON *command = cJSON_GetObjectItem(root, "command");
    if (command != NULL && cJSON_IsString(command) && command->valuestring != NULL)
    {
        const char *cmd = command->valuestring;
        ESP_LOGI(TAG, "Received command: %s", cmd);
  
        if (cmd != NULL && strlen(cmd) > 0)
        {
            if (strcmp(cmd, "reboot") == 0)
            {
                if (xrestartTimer != NULL)
                {
                    xTimerStart(xrestartTimer, 0);
                }
                else
                {
                    ESP_LOGE(TAG, "Restart timer is NULL");
                }
            }
            else if (strcmp(cmd, "force_update_obd") == 0)
            {
                elm327_update_obd(true);
            }
            else if (strcmp(cmd, "set_rtc_time") == 0)
            {
                // Get time parameters from JSON
                cJSON *hour = cJSON_GetObjectItem(root, "hour");
                cJSON *min = cJSON_GetObjectItem(root, "min");
                cJSON *sec = cJSON_GetObjectItem(root, "sec");
                cJSON *year = cJSON_GetObjectItem(root, "year");
                cJSON *month = cJSON_GetObjectItem(root, "month");
                cJSON *day = cJSON_GetObjectItem(root, "day");
                cJSON *weekday = cJSON_GetObjectItem(root, "weekday");
                
                // Check if all required parameters are present and valid
                if (hour && min && sec && year && month && day && weekday &&
                    cJSON_IsNumber(hour) && cJSON_IsNumber(min) && cJSON_IsNumber(sec) &&
                    cJSON_IsNumber(year) && cJSON_IsNumber(month) && cJSON_IsNumber(day) &&
                    cJSON_IsNumber(weekday))
                {
                    // Set time
                    esp_err_t time_err = rtcm_set_time(
                        (uint8_t)hour->valueint, 
                        (uint8_t)min->valueint, 
                        (uint8_t)sec->valueint
                    );
                    
                    // Set date
                    esp_err_t date_err = rtcm_set_date(
                        (uint8_t)year->valueint,
                        (uint8_t)month->valueint,
                        (uint8_t)day->valueint,
                        (uint8_t)weekday->valueint
                    );
                    rtcm_sync_system_time_from_rtc();
                    if (time_err == ESP_OK && date_err == ESP_OK) {
                        ESP_LOGI(TAG, "RTC time set successfully");
                    } else {
                        ESP_LOGE(TAG, "Failed to set RTC time: time_err=%d, date_err=%d", 
                                time_err, date_err);
                    }
                }
                else
                {
                    ESP_LOGE(TAG, "Missing or invalid parameters for set_rtc_time command");
                }
            }
        }
        else
        {
            ESP_LOGE(TAG, "Empty command received");
        }
    }
    else
    {
        ESP_LOGE(TAG, "Invalid or missing command");
    }
    cJSON_Delete(root);
    free(buf);

    const char *resp_str = "Command executed";
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}


static esp_err_t logo_handler(httpd_req_t *req)
{
    size_t logo_size = logo_end - logo_start;
	
	httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_send(req, (const char*)logo_start, logo_size);
    return ESP_OK;
}

static esp_err_t store_auto_data_handler(httpd_req_t *req)
{
	ESP_LOGI(TAG, "store_auto_data_handler called: content_len=%d", req ? req->content_len : -1);

	if (req == NULL)
	{
		return ESP_ERR_INVALID_ARG;
	}

	int total_len = req->content_len;

	// Validate content length
	if (total_len <= 0 || total_len > MAX_FILE_SIZE)
	{
		ESP_LOGE(TAG, "Invalid content length: %d (max %s)", total_len, MAX_FILE_SIZE_STR);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
		return ESP_FAIL;
	}

	// Allocate buffer for entire JSON content in PSRAM (+1 for null terminator)
	char *json_buffer = (char *)heap_caps_malloc(total_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (json_buffer == NULL)
	{
		ESP_LOGE(TAG, "Failed to allocate %d bytes in PSRAM for JSON buffer", total_len + 1);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
		return ESP_ERR_NO_MEM;
	}

	int received = 0;
	esp_err_t ret_val = ESP_OK;

	// Receive all data in chunks; retry on timeout
	while (received < total_len)
	{
		int to_read = MIN(4096, total_len - received);
		int r = httpd_req_recv(req, json_buffer + received, to_read);

		if (r < 0)
		{
			if (r == HTTPD_SOCK_ERR_TIMEOUT)
			{
				continue; // retry on timeout
			}
			ESP_LOGE(TAG, "Receive error: %d after %d/%d bytes", r, received, total_len);
			ret_val = ESP_FAIL;
			break;
		}

		if (r == 0)
		{
			ESP_LOGE(TAG, "Connection closed unexpectedly after %d/%d bytes", received, total_len);
			ret_val = ESP_FAIL;
			break;
		}

		received += r;
		ESP_LOGD(TAG, "Received chunk: %d bytes (total %d/%d)", r, received, total_len);
	}

	if (ret_val != ESP_OK)
	{
		free(json_buffer);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
		return ESP_FAIL;
	}

	// Null-terminate the JSON string
	json_buffer[received] = '\0';

	// Validate JSON format
	cJSON *json = cJSON_Parse(json_buffer);
	if (json == NULL)
	{
		const char *err_ptr = cJSON_GetErrorPtr();
		if (err_ptr)
		{
			ESP_LOGE(TAG, "Invalid JSON format near: %.32s", err_ptr);
		}
		else
		{
			ESP_LOGE(TAG, "Invalid JSON format (no error pointer)");
		}
		free(json_buffer);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
		return ESP_FAIL;
	}
	cJSON_Delete(json);

	ESP_LOGI(TAG, "Validated JSON payload, size=%d bytes", received);

	// Write to file under the autopid file lock (P2, issue #39): serializes against the
	// live reload's count+parse so a reload can never observe a torn/half-written file
	// (which would size pids[] from one generation and parse the other -> heap overflow).
	autopid_file_lock();
	FILE *f = fopen(FS_MOUNT_POINT "/auto_pid.json", "w");
	if (f == NULL)
	{
		autopid_file_unlock();
		ESP_LOGE(TAG, "Failed to open %s for writing", FS_MOUNT_POINT "/auto_pid.json");
		free(json_buffer);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file for writing");
		return ESP_FAIL;
	}

	size_t written = fwrite(json_buffer, 1, (size_t)received, f);
	if (written != (size_t)received)
	{
		ESP_LOGE(TAG, "File write failed: %zu/%d bytes", written, received);
		fclose(f);
		autopid_file_unlock();
		free(json_buffer);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write data to file");
		return ESP_FAIL;
	}

	fclose(f);
	autopid_file_unlock();
	free(json_buffer);

	// ---- Honest envelope (issue #39) -----------------------------------------
	// The file is written, so the new table applies at next boot regardless. Decide
	// whether it ALSO applies live:
	//   !queued            -> not POLL_LOG mode: only a reboot can load it.
	//   trip open          -> defer: the swap runs when the current CSV trip closes
	//                         (its columns stay frozen mid-trip) or on reboot.
	//   otherwise          -> live: the swap runs at the poll task's next safe point.
	bool queued = poll_log_request_reload();
	if (!queued)
	{
		config_server_send_apply_envelope(req, true, "reboot",
			"PID table saved; takes effect after reboot.");
	}
	else if (csv_logger_session_active())
	{
		config_server_send_apply_envelope(req, false, "deferred",
			"Datalog trip in progress -- new PID table takes effect when this trip ends (or on reboot).");
	}
	else
	{
		config_server_send_apply_envelope(req, false, "live",
			"PID table applied. New logging columns start with the next trip.");
	}
	ESP_LOGI(TAG, "store_auto_data_handler completed: written=%d bytes, queued=%d", received, (int)queued);

	return ESP_OK;
}

static void config_server_add_restart_tracker_status(cJSON *root)
{
	restart_tracker_state_t state = {0};
	restart_tracker_record_t latest = {0};
	const char *last_reset_reason = "unknown";
	const char *last_planned_reason = "none";
	const char *last_source = "unknown";
	double last_boot_timestamp_unix = 0;

	if (root == NULL)
	{
		return;
	}

	if (restart_tracker_get_state(&state) == ESP_OK)
	{
		cJSON_AddNumberToObject(root, "restart_boot_count", state.boot_count);
		cJSON_AddNumberToObject(root, "restart_unexpected_reset_count", state.unexpected_reset_count);
	}
	else
	{
		cJSON_AddNumberToObject(root, "restart_boot_count", 0);
		cJSON_AddNumberToObject(root, "restart_unexpected_reset_count", 0);
	}

	if (restart_tracker_get_latest_record(&latest) == ESP_OK)
	{
		last_reset_reason = restart_tracker_reset_reason_to_str((esp_reset_reason_t)latest.actual_reset_reason);
		if (latest.was_planned != 0U)
		{
			last_planned_reason = restart_tracker_planned_reason_to_str((restart_tracker_planned_reason_t)latest.planned_reason);
			last_source = restart_tracker_source_to_str((restart_tracker_source_t)latest.source);
		}
		if (latest.time_valid != 0U && latest.boot_timestamp > 0)
		{
			last_boot_timestamp_unix = (double)latest.boot_timestamp;
		}
	}

	cJSON_AddStringToObject(root, "restart_last_reset_reason", last_reset_reason);
	cJSON_AddStringToObject(root, "restart_last_planned_reason", last_planned_reason);
	cJSON_AddStringToObject(root, "restart_last_source", last_source);
	cJSON_AddNumberToObject(root, "restart_last_boot_timestamp_unix", last_boot_timestamp_unix);
}

char *config_server_get_status_json(bool remove_sensitive_info)
{
	char ip_str[20] = {0};

	const char *ip = wifi_mgr_get_sta_ip();
	strlcpy(ip_str, ip ? ip : "", sizeof(ip_str));
	cJSON *root = cJSON_CreateObject();
	char fver[16];
	char hver[32];

    esp_app_desc_t* running_app_info = dev_status_get_running_app_info();
	uint32_t firmware_ver_minor = 0, firmware_ver_major = 0;

	if (sscanf(running_app_info->version, "v%ld.%ld", &firmware_ver_major, &firmware_ver_minor) == 2) 
	{
		ESP_LOGI(TAG, "Firmware version: %ld.%ld", firmware_ver_major, firmware_ver_minor);
	} 

    sprintf(fver, "%ld.%02ld", firmware_ver_major, firmware_ver_minor);
    sprintf(hver, "WiCAN-%s", HARDWARE_VERSION);

	cJSON_AddStringToObject(root, "wifi_mode", device_config.wifi_mode);
	cJSON_AddStringToObject(root, "ap_ch", device_config.ap_ch);
	cJSON_AddStringToObject(root, "ap_ssid_en", device_config.ap_ssid_en);
	cJSON_AddStringToObject(root, "ap_ssid", device_config.ap_ssid);
	cJSON_AddStringToObject(root, "ap_auto_disable", device_config.ap_auto_disable);
	// Redaction is per-field, not per-block: an SSID or a security mode is not a
	// secret and the UI needs it to render, so only the passwords are replaced.
	// The old all-or-nothing guard dropped the whole group, which is a second
	// reason it could never be switched on -- doing so blanked the SSID too.
	cJSON_AddStringToObject(root, "sta_ssid", device_config.sta_ssid);
	config_add_secret(root, "sta_pass", device_config.sta_pass, remove_sensitive_info);
	cJSON_AddStringToObject(root, "sta_security", device_config.sta_security);
	cJSON_AddStringToObject(root, "home_ssid", device_config.home_ssid);
	config_add_secret(root, "home_password", device_config.home_password, remove_sensitive_info);
	cJSON_AddStringToObject(root, "home_security", device_config.home_security);
	cJSON_AddStringToObject(root, "drive_ssid", device_config.drive_ssid);
	config_add_secret(root, "drive_password", device_config.drive_password, remove_sensitive_info);
	cJSON_AddStringToObject(root, "drive_security", device_config.drive_security);
	cJSON_AddStringToObject(root, "home_protocol", device_config.home_protocol);
	cJSON_AddStringToObject(root, "drive_protocol", device_config.drive_protocol);
	cJSON_AddStringToObject(root, "drive_connection_type", device_config.drive_connection_type);
	cJSON_AddStringToObject(root, "drive_mode_timeout", device_config.drive_mode_timeout);
	cJSON_AddStringToObject(root, "sta_status", (wifi_mgr_is_sta_connected()?"Connected":"Not Connected"));
	// The device's own LAN address, shown on the Status tab (main.js checkStatus).
	// Not a credential, and the caller is already talking to it.
	cJSON_AddStringToObject(root, "sta_ip", ip_str);
	cJSON_AddStringToObject(root, "mdns", wc_mdns_get_hostname());
	cJSON_AddStringToObject(root, "ble_status", device_config.ble_status);
	cJSON_AddStringToObject(root, "ble_power", device_config.ble_power);
//	cJSON_AddStringToObject(root, "can_datarate", device_config.can_datarate);
	cJSON_AddStringToObject(root, "can_datarate", can_datarate_str[can_get_bitrate()]);
	cJSON_AddStringToObject(root, "can_mode", device_config.can_mode);
	cJSON_AddStringToObject(root, "port_type", device_config.port_type);
	cJSON_AddStringToObject(root, "port", device_config.port);
	cJSON_AddStringToObject(root, "fw_version", fver);
	cJSON_AddStringToObject(root, "hw_version", hver);
	cJSON_AddStringToObject(root, "git_version", GIT_SHA);
	cJSON_AddStringToObject(root, "protocol", device_config.protocol);
	cJSON_AddStringToObject(root, "sleep_status", device_config.sleep_status);
	cJSON_AddStringToObject(root, "can_wake", device_config.can_wake);
	cJSON_AddStringToObject(root, "sleep_disable_agree", device_config.sleep_disable_agree);
	cJSON_AddStringToObject(root, "sleep_volt", device_config.sleep_volt);
	cJSON_AddStringToObject(root, "engine_volt", device_config.engine_volt);
	cJSON_AddStringToObject(root, "sleep_time", device_config.sleep_time);
	cJSON_AddStringToObject(root, "wakeup_volt", device_config.wakeup_volt);
	cJSON_AddStringToObject(root, "periodic_wakeup", device_config.periodic_wakeup);
	cJSON_AddStringToObject(root, "wakeup_interval", device_config.wakeup_interval);

	cJSON_AddStringToObject(root, "batt_alert", device_config.batt_alert);
	cJSON_AddStringToObject(root, "batt_alert_ssid", device_config.batt_alert_ssid);
	config_add_secret(root, "batt_alert_pass", device_config.batt_alert_pass, remove_sensitive_info);
	cJSON_AddStringToObject(root, "batt_alert_url", device_config.batt_alert_url);
	cJSON_AddStringToObject(root, "batt_alert_port", device_config.batt_alert_port);
	cJSON_AddStringToObject(root, "batt_mqtt_user", device_config.batt_mqtt_user);
	config_add_secret(root, "batt_mqtt_pass", device_config.batt_mqtt_pass, remove_sensitive_info);
	cJSON_AddStringToObject(root, "batt_alert_protocol", device_config.batt_alert_protocol);
	cJSON_AddStringToObject(root, "batt_alert_volt", device_config.batt_alert_volt);
	cJSON_AddStringToObject(root, "batt_alert_topic", device_config.batt_alert_topic);
	cJSON_AddStringToObject(root, "batt_alert_time", device_config.batt_alert_time);
	cJSON_AddStringToObject(root, "csv_log", device_config.csv_log);
	cJSON_AddStringToObject(root, "log_filesystem", device_config.log_filesystem);
	cJSON_AddStringToObject(root, "log_period", device_config.log_period);
	cJSON_AddStringToObject(root, "csv_grid_hz", device_config.csv_grid_hz);
	cJSON_AddStringToObject(root, "csv_require_engine", device_config.csv_require_engine);
	cJSON_AddStringToObject(root, "log_storage", device_config.log_storage);
	// "mounted" | "unreadable" | "absent". The card is never auto-formatted, so
	// an unreadable one keeps the user's logs intact -- this is what tells them
	// it happened, on a device with no console.
	cJSON_AddStringToObject(root, "sd_status", sdcard_status_str());
	cJSON_AddStringToObject(root, "imu_threshold", device_config.imu_threshold);
	cJSON_AddStringToObject(root, "led_blink", device_config.led_blink);
	if(gpio_get_level(OBD_READY_PIN) == 1)
	{
		cJSON_AddStringToObject(root, "obd_chip_status", "Sleep");
	}
	else
	{
		cJSON_AddStringToObject(root, "obd_chip_status", "Ready");
	}
	// LED activity indicator state (issue #19): remote units have no visible
	// LED, and this is what lets the flash/datalog indications be verified
	// over HTTP.
	cJSON_AddStringToObject(root, "led_indicator", led_indicator_get_state_str());
	char uptime_str[32];
	dev_status_format_uptime(uptime_str, sizeof(uptime_str));
	if(uptime_str[0] == '\0')
	{
		strlcpy(uptime_str, "N/A", sizeof(uptime_str));
	}
	uptime_str[sizeof(uptime_str) - 1] = '\0';
	cJSON_AddStringToObject(root, "uptime", uptime_str);
	config_server_add_restart_tracker_status(root);

	// Add timestamp (Unix epoch in seconds)
	time_t now;
	time(&now);
	cJSON_AddNumberToObject(root, "timestamp", (double)now);

	// Time sync status (DNS resolution stability)
	cJSON_AddBoolToObject(root, "time_synced", dev_status_is_time_synced());

	// STA DNS servers
	{
		char dns_main[48] = {0};
		char dns_backup[48] = {0};
		wifi_mgr_get_sta_dns(dns_main, sizeof(dns_main), dns_backup, sizeof(dns_backup));
		cJSON_AddStringToObject(root, "dns_main", dns_main[0] ? dns_main : "N/A");
		cJSON_AddStringToObject(root, "dns_backup", dns_backup[0] ? dns_backup : "N/A");
	}

	char volt[8]= {0};
	float tmp = 0;
	sleep_mode_get_voltage(&tmp);
	/* Two decimals, not one: the reading is no longer rounded to 0.1 V, and this endpoint is the
	 * only way to watch the battery on a real car -- there is no serial console on OBD-PRO. */
	snprintf(volt, sizeof(volt), "%.2fV", tmp);
	cJSON_AddStringToObject(root, "batt_voltage", volt);

	cJSON_AddStringToObject(root, "device_id", device_id);
	cJSON_AddStringToObject(root, "subnet_overlap", dev_status_is_bit_set(DEV_STA_AP_OVERLAP_BIT) ? "yes" : "no");

	if(autopid_get_ecu_status())
	{
		cJSON_AddStringToObject(root, "ecu_status", "online");
	}
	else
	{
		cJSON_AddStringToObject(root, "ecu_status", "offline");
	}

    char *resp_str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return resp_str;
}

static esp_err_t check_status_handler(httpd_req_t *req)
{
	// Redacted: passwords come back as CONFIG_SECRET_PLACEHOLDER. Nothing in the
	// UI reads a credential from this endpoint (main.js checkStatus only renders
	// status fields), so this costs nothing and closes the larger of the two leaks.
	char *resp_str = config_server_get_status_json(true);

	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    free((void *)resp_str);
    
    return ESP_OK;
}

typedef struct {
	esp_ota_handle_t update_handle;
	const esp_partition_t *update_partition;
	bool started;
	bool completed;
	size_t total_size;
	esp_err_t err;
} ota_upload_ctx_t;

static bool ota_on_part_begin(const multipart_part_info_t *info, void *user_ctx)
{
	ota_upload_ctx_t *ctx = (ota_upload_ctx_t *)user_ctx;
	if (!ctx || ctx->started) return false;

	// Accept the first file-like part
	if (!info) return false;
	if (info->filename[0] == '\0' && strcasecmp(info->content_type, "application/octet-stream") != 0)
	{
		return false;
	}

	ESP_LOGI(TAG, "OTA multipart part: name='%s' filename='%s' ctype='%s'", info->name, info->filename, info->content_type);

	const esp_partition_t *running = esp_ota_get_running_partition();
	const esp_partition_t *configured = esp_ota_get_boot_partition();
	if (configured != running)
	{
		ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08lx, but running from offset 0x%08lx",
				 configured->address, running->address);
	}

	ctx->update_partition = esp_ota_get_next_update_partition(NULL);
	if (!ctx->update_partition)
	{
		ctx->err = ESP_FAIL;
		return false;
	}

	ESP_LOGI(TAG, "Writing OTA to partition subtype %d at offset 0x%lx",
			 ctx->update_partition->subtype, ctx->update_partition->address);

	ctx->err = esp_ota_begin(ctx->update_partition, OTA_SIZE_UNKNOWN, &ctx->update_handle);
	if (ctx->err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(ctx->err));
		return false;
	}

	ctx->started = true;
	// Operational event (Task #24): firmware update began. Runs on the httpd task; the in-RAM ring
	// carries it even though the SD may be unavailable during the OTA flash window.
	event_log_emit(EVL_OTA_START, "firmware OTA started (part subtype %d)", (int)ctx->update_partition->subtype);
	return true;
}

static esp_err_t ota_on_part_data(const char *data, size_t len, void *user_ctx)
{
	ota_upload_ctx_t *ctx = (ota_upload_ctx_t *)user_ctx;
	if (!ctx || !ctx->started) return ESP_FAIL;
	if (len == 0) return ESP_OK;

	ctx->err = esp_ota_write(ctx->update_handle, data, len);
	if (ctx->err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(ctx->err));
		return ctx->err;
	}
	ctx->total_size += len;
	return ESP_OK;
}

static void ota_on_part_end(void *user_ctx)
{
	ota_upload_ctx_t *ctx = (ota_upload_ctx_t *)user_ctx;
	if (!ctx) return;
	ctx->completed = true;
}

static void ota_on_finished(void *user_ctx)
{
	ota_upload_ctx_t *ctx = (ota_upload_ctx_t *)user_ctx;
	if (!ctx || !ctx->started) return;

	ctx->err = esp_ota_end(ctx->update_handle);
	if (ctx->err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(ctx->err));
		return;
	}

	ctx->err = esp_ota_set_boot_partition(ctx->update_partition);
	if (ctx->err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(ctx->err));
		return;
	}
}

typedef struct {
	FILE *fd;
	const char *path;
	size_t total_size;
	esp_err_t err;
	bool started;
} file_upload_ctx_t;

static bool file_on_part_begin(const multipart_part_info_t *info, void *user_ctx)
{
	file_upload_ctx_t *ctx = (file_upload_ctx_t *)user_ctx;
	if (!ctx || ctx->started) return false;
	if (!info) return false;
	if (info->filename[0] == '\0') return false; // only accept file parts

	ctx->fd = fopen(ctx->path, "w");
	if (!ctx->fd)
	{
		ESP_LOGE(TAG, "Failed to open %s for writing", ctx->path);
		ctx->err = ESP_FAIL;
		return false;
	}
	ctx->started = true;
	ESP_LOGI(TAG, "File multipart part: name='%s' filename='%s' -> %s", info->name, info->filename, ctx->path);
	return true;
}

static esp_err_t file_on_part_data(const char *data, size_t len, void *user_ctx)
{
	file_upload_ctx_t *ctx = (file_upload_ctx_t *)user_ctx;
	if (!ctx || !ctx->fd) return ESP_FAIL;
	if (len == 0) return ESP_OK;
	if (fwrite(data, 1, len, ctx->fd) != len)
	{
		ESP_LOGE(TAG, "File write failed");
		ctx->err = ESP_FAIL;
		return ESP_FAIL;
	}
	ctx->total_size += len;
	return ESP_OK;
}

static void file_on_part_end(void *user_ctx)
{
	file_upload_ctx_t *ctx = (file_upload_ctx_t *)user_ctx;
	if (!ctx) return;
	if (ctx->fd)
	{
		fclose(ctx->fd);
		ctx->fd = NULL;
	}
}

static void file_on_finished(void *user_ctx)
{
	(void)user_ctx;
}

/* Handler to upload a file onto the server */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
	uint32_t total_size = 0;

    if(config_server_get_ble_config())
    {
    	ble_disable();
    }
    can_disable();
    /* Skip leading "/upload" from URI to get filename */
    /* Note sizeof() counts NULL termination hence the -1 */
    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri + sizeof("/upload") - 1, sizeof(filepath));
    if (!filename) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* Filename cannot have a trailing '/' */
    if (filename[strlen(filename) - 1] == '/') {
        ESP_LOGE(TAG, "Invalid filename : %s", filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
        return ESP_FAIL;
    }

    // /* File cannot be larger than a limit */
    // if (req->content_len > MAX_FILE_SIZE)
    // {
    //     ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
    //     /* Respond with 400 Bad Request */
    //     httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
    //                         "File size must be less than "
    //                         MAX_FILE_SIZE_STR "!");
    //     /* Return failure to close underlying connection else the
    //      * incoming file content will keep the socket busy */
    //     return ESP_FAIL;
    // }

    ESP_LOGI(TAG, "Receiving file : %s...", filename);

	ota_upload_ctx_t ctx = {0};
	ctx.err = ESP_OK;

	multipart_upload_handlers_t handlers = {
		.on_part_begin = ota_on_part_begin,
		.on_part_data = ota_on_part_data,
		.on_part_end = ota_on_part_end,
		.on_finished = ota_on_finished,
	};

	multipart_upload_config_t mp_cfg = multipart_upload_default_config();
	mp_cfg.rx_buf_size = 4096;

	/* Hold sleep off for the whole upload. Without this the sleep countdown can expire mid-transfer
	 * and run wifi_mgr_deinit() straight through this live HTTP request, which crashed the bench
	 * device. Raised BEFORE the transfer and lowered on every exit path below. */
	config_server_ota_active_set(true);

	esp_err_t mp_err = multipart_upload_handle(req, &handlers, &ctx, &mp_cfg);
	if (mp_err != ESP_OK || ctx.err != ESP_OK || !ctx.started)
	{
		config_server_ota_active_set(false);
		if (ctx.started && ctx.err != ESP_OK)
		{
			esp_ota_abort(ctx.update_handle);
		}
		// Operational event (Task #24): single consolidated OTA-failure site.
		event_log_emit(EVL_OTA_FAIL, "OTA failed: mp=%s ota=%s started=%d",
		               esp_err_to_name(mp_err), esp_err_to_name(ctx.err), (int)ctx.started);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA upload failed");
		return ESP_FAIL;
	}

	/* Transfer done and the boot partition is already switched. Stay raised until the scheduled
	 * reboot below actually fires -- there is nothing left to protect, but lowering it here would
	 * let a sleep slip in during the response + reboot window for no benefit. It is cleared by
	 * the reboot itself. */
	total_size = (uint32_t)ctx.total_size;
	ESP_LOGI(TAG, "OTA upload complete: %lu bytes", (unsigned long)total_size);
	// Operational event (Task #24): OTA written + boot partition switched; reboot scheduled below.
	event_log_emit(EVL_OTA_OK, "firmware OTA complete: %lu bytes, reboot scheduled", (unsigned long)total_size);

	httpd_resp_set_status(req, "303 See Other");
	httpd_resp_set_hdr(req, "Location", "/");
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
	httpd_resp_set_hdr(req, "Connection", "close");
#endif
	httpd_resp_sendstr(req, "File uploaded successfully");

	// Reboot after responding
	config_server_schedule_reboot(RESTART_TRACKER_PLANNED_REASON_OTA_APPLY,
						 RESTART_TRACKER_SOURCE_WEB_UI,
						 RESTART_TRACKER_FLAG_FIRMWARE_UPDATED);
	return ESP_OK;
}

/* ---- NC Flash SD-staged flash upload: POST /upload/sd/<name> --------------
 * Stores a staged flash image (checksum-corrected ROM ++ SBL, produced host-side
 * by wican_sd_package.py) to /sdcard/roms/<name> over reliable TCP, then reports
 * {bytes_written, crc32}. The host (wican_sd_upload.py) verifies that CRC before
 * it will trigger a flash, so a corrupt upload can never reach the ECU. This is
 * additive and NON-destructive: no CAN/BLE disable, no reboot. Writes to a .part
 * temp then atomically renames so a half-upload never looks complete. */
#define SD_ROMS_DIR        SD_CARD_MOUNT_POINT "/roms"
#define MAX_SD_UPLOAD_SIZE (4u * 1024u * 1024u) /* generous; staged image is ~1.03 MB */

/* Reflected CRC-32 step (poly 0xEDB88320), no init/final XOR — the caller seeds
 * with 0xFFFFFFFF and XORs the final result with 0xFFFFFFFF, so it matches
 * Python's zlib.crc32 byte-for-byte (the host's verification CRC). */
static uint32_t nc_crc32_step(uint32_t crc, const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++)
	{
		crc ^= data[i];
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return crc;
}

typedef struct {
	FILE *fd;
	const char *path;   /* temp (.part) path we stream into */
	size_t total_size;
	uint32_t crc;       /* running zlib CRC state, seeded 0xFFFFFFFF */
	esp_err_t err;
	bool started;
	bool too_big;
} sd_upload_ctx_t;

static bool sd_on_part_begin(const multipart_part_info_t *info, void *user_ctx)
{
	sd_upload_ctx_t *ctx = (sd_upload_ctx_t *)user_ctx;
	if (!ctx || ctx->started || !info) return false;
	if (info->filename[0] == '\0') return false; /* only accept file parts */
	ctx->fd = fopen(ctx->path, "w");
	if (!ctx->fd)
	{
		ESP_LOGE(TAG, "SD upload: cannot open %s", ctx->path);
		ctx->err = ESP_FAIL;
		return false;
	}
	ctx->started = true;
	return true;
}

static esp_err_t sd_on_part_data(const char *data, size_t len, void *user_ctx)
{
	sd_upload_ctx_t *ctx = (sd_upload_ctx_t *)user_ctx;
	if (!ctx || !ctx->fd) return ESP_FAIL;
	if (len == 0) return ESP_OK;
	if (ctx->total_size + len > MAX_SD_UPLOAD_SIZE)
	{
		ctx->too_big = true;
		ctx->err = ESP_FAIL;
		return ESP_FAIL;
	}
	if (fwrite(data, 1, len, ctx->fd) != len)
	{
		ESP_LOGE(TAG, "SD upload: write failed");
		ctx->err = ESP_FAIL;
		return ESP_FAIL;
	}
	ctx->crc = nc_crc32_step(ctx->crc, (const uint8_t *)data, len);
	ctx->total_size += len;
	return ESP_OK;
}

static void sd_on_part_end(void *user_ctx)
{
	sd_upload_ctx_t *ctx = (sd_upload_ctx_t *)user_ctx;
	if (ctx && ctx->fd) { fclose(ctx->fd); ctx->fd = NULL; }
}

/* Validate the <name> after /upload/sd/. Returns 0 only for a simple filename
 * with an extension and no path separators / traversal / control characters. */
static int sd_name_is_safe(const char *name)
{
	if (!name || name[0] == '\0' || name[0] == '.') return -1;
	size_t n = strlen(name);
	if (n > 96) return -1;
	if (name[n - 1] == '/') return -1;
	bool has_dot = false;
	for (size_t i = 0; i < n; i++)
	{
		char c = name[i];
		if (c == '/' || c == '\\') return -1;                       /* no separators */
		if (c == '.' && i + 1 < n && name[i + 1] == '.') return -1; /* no ".." */
		if (c == '.') has_dot = true;
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
		if (!ok) return -1;
	}
	return has_dot ? 0 : -1; /* require an extension */
}

static esp_err_t upload_sd_handler(httpd_req_t *req)
{
	const char *prefix = "/upload/sd/";
	if (strncmp(req->uri, prefix, strlen(prefix)) != 0)
	{
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
		return ESP_FAIL;
	}

	/* Extract <name> (drop any query string) and validate it. */
	const char *name = req->uri + strlen(prefix);
	char namebuf[97];
	const char *q = strchr(name, '?');
	size_t nlen = q ? (size_t)(q - name) : strlen(name);
	if (nlen >= sizeof(namebuf))
	{
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Name too long");
		return ESP_FAIL;
	}
	memcpy(namebuf, name, nlen);
	namebuf[nlen] = '\0';
	if (sd_name_is_safe(namebuf) != 0)
	{
		ESP_LOGE(TAG, "SD upload: rejected unsafe name '%s'", namebuf);
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
		return ESP_FAIL;
	}

	if (!sdcard_is_mounted())
	{
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card not mounted");
		return ESP_FAIL;
	}

	/* Ensure /sdcard/roms exists. */
	struct stat st;
	if (stat(SD_ROMS_DIR, &st) != 0 && mkdir(SD_ROMS_DIR, 0775) != 0)
	{
		ESP_LOGE(TAG, "SD upload: mkdir %s failed", SD_ROMS_DIR);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create roms dir");
		return ESP_FAIL;
	}

	/* Big enough for SD_ROMS_DIR + the full 96-char namebuf + the ".part" temp
	 * suffix (FILE_PATH_MAX ~= 47 is too small — a ~29+ char name overflowed). */
	char finalpath[160];
	char tmppath[160];
	int fn = snprintf(finalpath, sizeof(finalpath), "%s/%s", SD_ROMS_DIR, namebuf);
	int tn = snprintf(tmppath, sizeof(tmppath), "%s/%s.part", SD_ROMS_DIR, namebuf);
	if (fn <= 0 || fn >= (int)sizeof(finalpath) || tn <= 0 || tn >= (int)sizeof(tmppath))
	{
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Path too long");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "SD upload: receiving %s", finalpath);

	sd_upload_ctx_t ctx = {0};
	ctx.path = tmppath;
	ctx.crc = 0xFFFFFFFFu;
	ctx.err = ESP_OK;

	multipart_upload_handlers_t handlers = {
		.on_part_begin = sd_on_part_begin,
		.on_part_data = sd_on_part_data,
		.on_part_end = sd_on_part_end,
		.on_finished = file_on_finished,
	};
	multipart_upload_config_t mp_cfg = multipart_upload_default_config();
	mp_cfg.rx_buf_size = 4096;

	esp_err_t mp_err = multipart_upload_handle(req, &handlers, &ctx, &mp_cfg);
	if (ctx.fd) { fclose(ctx.fd); ctx.fd = NULL; }

	if (mp_err != ESP_OK || ctx.err != ESP_OK || !ctx.started)
	{
		unlink(tmppath);
		if (ctx.too_big)
			httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large");
		else
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
		return ESP_FAIL;
	}

	/* Atomic publish: replace any prior staged copy of the same name. */
	unlink(finalpath);
	if (rename(tmppath, finalpath) != 0)
	{
		ESP_LOGE(TAG, "SD upload: rename %s -> %s failed", tmppath, finalpath);
		unlink(tmppath);
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
		return ESP_FAIL;
	}

	uint32_t crc = ctx.crc ^ 0xFFFFFFFFu;
	ESP_LOGI(TAG, "SD upload complete: %s %u bytes crc32=0x%08X",
	         finalpath, (unsigned)ctx.total_size, (unsigned)crc);

	char resp[96];
	int rn = snprintf(resp, sizeof(resp), "{\"bytes_written\":%u,\"crc32\":%u}",
	                  (unsigned)ctx.total_size, (unsigned)crc);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, resp, rn);
	return ESP_OK;
}

#define MAX_AVAILABLE_PIDS_SIZE 		(1024*15)
static esp_err_t scan_available_pids_handler(httpd_req_t *req)
{
    char protocol[8];
    char param[32];
    uint8_t protocol_num = 6; // Default protocol

    if(config_server_protocol() != AUTO_PID)
    {
        httpd_resp_set_type(req, "application/json");
		const char *resp_str = "{\"text\":\"Go to Settings -> CAN and set Protocol to AutoPID then click Submit Changes\"}";
        httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
	
    if (httpd_req_get_url_query_str(req, param, sizeof(param)) == ESP_OK) {
        if (httpd_query_key_value(param, "protocol", protocol, sizeof(protocol)) == ESP_OK) {
            protocol_num = atoi(protocol);
            ESP_LOGI(TAG, "Scanning PIDs with protocol: %d", protocol_num);
        }
    }

    // char *available_pids = malloc(MAX_AVAILABLE_PIDS_SIZE);
    char *available_pids = heap_caps_malloc(MAX_AVAILABLE_PIDS_SIZE, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (available_pids == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    memset(available_pids, 0, MAX_AVAILABLE_PIDS_SIZE);
    
    if (autopid_find_standard_pid(protocol_num, available_pids, MAX_AVAILABLE_PIDS_SIZE) == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, available_pids, HTTPD_RESP_USE_STRLEN);
    }

    free(available_pids);
    return ESP_OK;
}

static esp_err_t std_pid_info_handler(httpd_req_t *req) 
{
    char *pid_info = get_standard_pids_json();
    if (pid_info == NULL) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, pid_info, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = NULL
};
static const httpd_uri_t store_config_uri = {
    .uri       = "/store_config",
    .method    = HTTP_POST,
    .handler   = store_config_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = NULL
};
static const httpd_uri_t load_pid_auto_uri = {
    .uri       = "/load_auto_pid",
    .method    = HTTP_GET,
    .handler   = load_pid_auto_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = NULL
};
static const httpd_uri_t check_status_uri = {
    .uri       = "/check_status",
    .method    = HTTP_GET,
    .handler   = check_status_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = NULL
};
static const httpd_uri_t load_config_uri = {
    .uri       = "/load_config",
    .method    = HTTP_GET,
    .handler   = load_config_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = NULL
};
static const httpd_uri_t logo_uri = {
    .uri       = "/logo.svg",
    .method    = HTTP_GET,
    .handler   = logo_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = NULL
};
static struct file_server_data server_data = {.base_path = FS_MOUNT_POINT""};
//static struct file_server_data *server_data = NULL;
/* URI handler for uploading files to server */
static const httpd_uri_t file_upload = {
    .uri       = "/upload/ota.bin",   // Match all URIs of type /upload/path/to/file
    .method    = HTTP_POST,
    .handler   = upload_post_handler,
    .user_ctx  = &server_data    // Pass server data as context
};
static const httpd_uri_t system_reboot = {
    .uri       = "/system_reboot",   // Match all URIs of type /upload/path/to/file
    .method    = HTTP_POST,
    .handler   = system_reboot_handler,
    .user_ctx  = NULL    // Pass server data as context
};
static const httpd_uri_t store_auto_data_uri = {
    .uri       = "/store_auto_data",
    .method    = HTTP_POST,
    .handler   = store_auto_data_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = NULL
};
static const httpd_uri_t upload_sd_uri = {
    .uri       = "/upload/sd/*",   // NC Flash SD-staged flash image upload
    .method    = HTTP_POST,
    .handler   = upload_sd_handler,
    .user_ctx  = &server_data
};
static const httpd_uri_t system_commands = {
    .uri       = "/system_commands",   // Match all URIs of type /upload/path/to/file
    .method    = HTTP_POST,
    .handler   = system_commands_handler,
    .user_ctx  = NULL    // Pass server data as context
};
static const httpd_uri_t scan_available_pids_uri = {
    .uri       = "/scan_available_pids",
    .method    = HTTP_GET,
    .handler   = scan_available_pids_handler,
    .user_ctx  = NULL
};
static const httpd_uri_t get_uri_common = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = get_uri_handler,
    .user_ctx  = &server_data 
};
static const httpd_uri_t std_pid_info = {
    .uri       = "/std_pid_info",
    .method    = HTTP_GET,
    .handler   = std_pid_info_handler,
    .user_ctx  = &server_data
};

/* GET /poll_status -> live POLL_LOG metrics (req/s, rtt, ok/timeout/txfail) over WiFi, so the
 * polling rate is readable in the car without a serial cable. Safe in any protocol mode. */
static esp_err_t poll_status_handler(httpd_req_t *req)
{
    char *status = poll_log_get_status_json();
    httpd_resp_set_type(req, "application/json");
    if (status == NULL)
    {
        httpd_resp_sendstr(req, "{\"active\":false}");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, status);
    free(status);
    return ESP_OK;
}
static const httpd_uri_t poll_status_uri = {
    .uri       = "/poll_status",
    .method    = HTTP_GET,
    .handler   = poll_status_handler,
    .user_ctx  = NULL
};

/* ------------------------------------------------------------------------------------------
 * PERMANENT diagnostic endpoint:  GET /wake_probe
 *
 * Kept deliberately. This box has no serial console (the USB-C port is a USB HOST at runtime), so
 * without this endpoint the sleep/resume path has NO observable surface at all and every future
 * investigation starts by guessing. It is a read-only GET on an HTTP server that already accepts
 * /store_config writes and firmware uploads, so gating this one route would be theater -- if auth
 * ever arrives it should cover the whole server.
 *
 * What each field is for:
 *
 *   chip        -- elm327_chip_get_status(), a raw read of OBD_READY_PIN. This is the ONLY way
 *                  to catch the sleep babysitter re-sleeping the interpreter chip right after a
 *                  resume. In poll_log protocol mode the ELM327 TCP port is not driven, so
 *                  talking ELM327 over the wire cannot distinguish "asleep" from "not in use".
 *   uptime_ms   -- proves a wake RESUMED (uptime keeps climbing) rather than rebooted.
 *   fence       -- the #88 sleep fence; must be false while awake or the producers stay parked.
 *   can_enabled -- the bus really came back.
 * ------------------------------------------------------------------------------------------ */
static esp_err_t wake_probe_handler(httpd_req_t *req)
{
    const elm327_chip_status_t chip = elm327_chip_get_status();

    /* Stack headroom of the sleep task, in BYTES remaining at its worst point. This is the number
     * that matters most: the resume runs the whole WiFi bring-up inside light_sleep_task, work
     * that used to happen only in app_main because waking always rebooted. If this trends toward
     * zero across wake cycles, the task is overflowing its static stack -- which corrupts whatever
     * sits next to it and panics with a jump to a nonsense address. */
    const TaskHandle_t sleep_task = xTaskGetHandle("sleep_task");   /* the task's real name */
    const unsigned sleep_hw = sleep_task ? (unsigned)(uxTaskGetStackHighWaterMark(sleep_task) * sizeof(StackType_t))
                                         : 0u;

    char body[320];
    snprintf(body, sizeof(body),
             "{\"uptime_ms\":%lld,\"chip\":\"%s\",\"fence\":%s,\"can_enabled\":%s,"
             "\"free_heap\":%u,\"largest_block\":%u,\"sleep_task_stack_free\":%u}",
             esp_timer_get_time() / 1000,
             (chip == ELM327_READY) ? "ready" : "sleep",
             can_sleep_fence_active() ? "true" : "false",
             can_is_enabled() ? "true" : "false",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             sleep_hw);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static const httpd_uri_t wake_probe_uri = {
    .uri       = "/wake_probe",
    .method    = HTTP_GET,
    .handler   = wake_probe_handler,
    .user_ctx  = NULL
};

/* ---- GET /sleep_status (#85) ------------------------------------------------------------------
 * Tiny live view of the sleep state machine, for the web UI's sleep-countdown banner. Deliberately
 * NOT folded into /check_status: that response is ~2 KB, rebuilds a dozen subsystem strings and
 * carries (redacted) WiFi credentials, and the banner has to poll from EVERY tab every couple of
 * seconds. This one is a stack buffer and a single non-blocking queue peek.
 *
 *   state       -- "off"       sleep is disabled in config, or the state machine has published
 *                              nothing yet. The publish site is gated on sleep_en == 1, so with
 *                              sleep off the peek simply fails -- which is what we report, and is
 *                              what stops the UI rendering "undefined".
 *                  "normal"    awake, no countdown running.
 *                  "countdown" counting down to sleep; secs_left is meaningful.
 *                  "sleeping" / "waking" -- present for completeness. Never observable in practice:
 *                              the radio is down through both, so nothing can answer this request.
 *   secs_left   -- whole seconds until sleep, rounded UP so it never reads 0 while still counting.
 *                  0 unless state is "countdown".
 *   secs_total  -- the countdown length in seconds, AS THE SLEEP TASK ARMED IT. The browser uses
 *                  (secs_total - secs_left) as "how long has this countdown been running", which is
 *                  how it suppresses the ~2-second countdown that every boot starts and cancels.
 *                  Taken from the published struct, NOT re-read from config here: the task and a
 *                  local read fall back to different values on a bad parse (120000 ms vs 0), and a
 *                  secs_total of 0 would make elapsed clamp to 0 so the banner never appeared.
 *   voltage     -- the reading that started the countdown, so the banner can say why.
 * ---------------------------------------------------------------------------------------------- */
static esp_err_t sleep_status_handler(httpd_req_t *req)
{
    sleep_state_info_t info = {0};
    const char *state_str = "off";
    unsigned secs_left = 0, secs_total = 0;
    float voltage = 0.0f;

    if (sleep_mode_get_state(&info) == ESP_OK)
    {
        voltage    = info.voltage;
        secs_total = (unsigned)(info.total_ms / 1000u);
        switch (info.state)
        {
            case STATE_NORMAL:       state_str = "normal";    break;
            case STATE_LOW_VOLTAGE:  state_str = "countdown";
                                     /* Round UP: 1..999 ms left must not render as "0:00". */
                                     secs_left = (unsigned)((info.timer + 999u) / 1000u);
                                     break;
            case STATE_SLEEPING:     state_str = "sleeping";  break;
            case STATE_WAKE_PENDING: state_str = "waking";    break;
        }
    }

    char body[160];
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"secs_left\":%u,\"secs_total\":%u,\"voltage\":%.2f}",
             state_str, secs_left, secs_total, (double)voltage);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static const httpd_uri_t sleep_status_uri = {
    .uri       = "/sleep_status",
    .method    = HTTP_GET,
    .handler   = sleep_status_handler,
    .user_ctx  = NULL
};

static bool config_server_parse_cfg_into(device_config_t *dst, const char *cfg)
{
	cJSON * root, *key = 0;
	root = cJSON_Parse(cfg);
	if (root == NULL) {
		ESP_LOGE(TAG, "Failed to parse JSON config");
		goto config_error_no_json;
	}

	// Initialize fallback list to empty before parsing
	dst->sta_fallbacks_count = 0;
	for (int i = 0; i < 5; ++i) {
		dst->sta_fallbacks[i].ssid[0] = '\0';
		dst->sta_fallbacks[i].pass[0] = '\0';
		strlcpy(dst->sta_fallbacks[i].security, "wpa3", sizeof(dst->sta_fallbacks[i].security));
	}
	
	key = cJSON_GetObjectItem(root,"wifi_mode");
	if(key == 0)
	{
		goto config_error;
	}
	if (key->valuestring == NULL) {
		goto config_error;
	}
	strlcpy(dst->wifi_mode, key->valuestring, sizeof(dst->wifi_mode));
	ESP_LOGI(TAG, "dst->wifi_mode: %s", dst->wifi_mode);

	key = cJSON_GetObjectItem(root,"ap_ch");
	if(key == 0)
	{
		goto config_error;
	}
	if (key->valuestring == NULL) {
		goto config_error;
	}
	strlcpy(dst->ap_ch, key->valuestring, sizeof(dst->ap_ch));
	ESP_LOGI(TAG, "dst->ap_ch: %s", dst->ap_ch);

	// Optional custom AP SSID (backward compatible)
	strlcpy(dst->ap_ssid_en, "disable", sizeof(dst->ap_ssid_en));
	dst->ap_ssid[0] = '\0';
	key = cJSON_GetObjectItem(root, "ap_ssid_en");
	if (key)
	{
		if (cJSON_IsString(key) && key->valuestring)
		{
			if (strcmp(key->valuestring, "enable") == 0 || strcmp(key->valuestring, "disable") == 0)
			{
				strlcpy(dst->ap_ssid_en, key->valuestring, sizeof(dst->ap_ssid_en));
			}
		}
		else if (cJSON_IsBool(key))
		{
			strlcpy(dst->ap_ssid_en, cJSON_IsTrue(key) ? "enable" : "disable", sizeof(dst->ap_ssid_en));
		}
	}
	ESP_LOGI(TAG, "dst->ap_ssid_en: %s", dst->ap_ssid_en);
	key = cJSON_GetObjectItem(root, "ap_ssid");
	if (key && cJSON_IsString(key) && key->valuestring)
	{
		strlcpy(dst->ap_ssid, key->valuestring, sizeof(dst->ap_ssid));
	}
	if (strcmp(dst->ap_ssid_en, "enable") == 0)
	{
		size_t ap_ssid_len = strlen(dst->ap_ssid);
		if (ap_ssid_len < AP_SSID_MIN_LEN || ap_ssid_len > AP_SSID_MAX_LEN)
		{
			ESP_LOGE(TAG, "Invalid ap_ssid length %u", (unsigned)ap_ssid_len);
			goto config_error;
		}
	}

	key = cJSON_GetObjectItem(root,"sta_ssid");
	if(key == 0)
	{
		goto config_error;
	}
	if(key->valuestring == NULL || strlen(key->valuestring) == 0 || strlen(key->valuestring) > 32)
	{
		goto config_error;
	}
	strlcpy(dst->sta_ssid, key->valuestring, sizeof(dst->sta_ssid));
	ESP_LOGI(TAG, "dst->sta_ssid: %s", dst->sta_ssid);

	key = cJSON_GetObjectItem(root,"sta_pass");
	if(key == 0)
	{
		goto config_error;
	}
	if(key->valuestring == NULL || strlen(key->valuestring) < 8 || strlen(key->valuestring) > 64)
	{
		goto config_error;
	}
	strlcpy(dst->sta_pass, key->valuestring, sizeof(dst->sta_pass));
	CONFIG_LOG_SECRET("dst->sta_pass", dst->sta_pass);

	key = cJSON_GetObjectItem(root,"can_datarate");
	if(key == 0)
	{
		goto config_error;
	}
	if (key->valuestring == NULL) {
		goto config_error;
	}
	strlcpy(dst->can_datarate, key->valuestring, sizeof(dst->can_datarate));
	ESP_LOGI(TAG, "dst->can_datarate: %s", dst->can_datarate);

	key = cJSON_GetObjectItem(root,"can_mode");
	if(key == 0)
	{
		goto config_error;
	}
	if (key->valuestring == NULL) {
		goto config_error;
	}
	strlcpy(dst->can_mode, key->valuestring, sizeof(dst->can_mode));
	ESP_LOGI(TAG, "dst->can_mode: %s", dst->can_mode);

	key = cJSON_GetObjectItem(root,"port_type");
	if(key == 0)
	{
		goto config_error;
	}
	if (key->valuestring == NULL) {
		goto config_error;
	}
	strlcpy(dst->port_type, key->valuestring, sizeof(dst->port_type));
	ESP_LOGI(TAG, "dst->port_type: %s", dst->port_type);

	key = cJSON_GetObjectItem(root,"port");
	if(key == 0)
	{
		goto config_error;
	}
	if (key->valuestring == NULL) {
		goto config_error;
	}
	strlcpy(dst->port, key->valuestring, sizeof(dst->port));
	ESP_LOGI(TAG, "dst->port: %s", dst->port);


	key = cJSON_GetObjectItem(root,"ap_pass");
	if(key == 0)
	{
		goto config_error;
	}
	if(key->valuestring == NULL || strlen(key->valuestring) < 8 || strlen(key->valuestring) > 64)
	{
		goto config_error;
	}
	strlcpy(dst->ap_pass, key->valuestring, sizeof(dst->ap_pass));
	CONFIG_LOG_SECRET("dst->ap_pass", dst->ap_pass);

	key = cJSON_GetObjectItem(root,"protocol");
	if(key == 0)
	{
		goto config_error;
	}
	if(key->valuestring == NULL || strlen(key->valuestring) < 2 || strlen(key->valuestring) > 64)
	{
		goto config_error;
	}
	strlcpy(dst->protocol, key->valuestring, sizeof(dst->protocol));
	// The legacy AutoPID scheduler is retired on this fork (issue #28). It polls one PID at a
	// time (~0.5 Hz per channel, vs poll_log sweeping the whole table ~20x/s) and it never runs
	// the calculated-channel pass -- autopid_eval_calculated_channels() is called only from
	// poll_log.c and fast_log.c -- so its CSVs carry phantom all-empty CALC columns. Devices
	// that still store the old value are coerced here, in RAM ONLY: no write to config.json, so
	// a booting device never risks the truncate-then-write path. Every consumer (boot, the
	// /store_config shadow validation, the live-apply diff) funnels through this parser, and the
	// coercion is idempotent. Unlike home/drive_protocol -- where the getter alone is enough --
	// this one rewrites the string, because /check_status reports it verbatim and the web UI
	// keys its "this protocol cannot record PIDs" banner off that value.
	if(strcmp(dst->protocol, "auto_pid") == 0)
	{
		strlcpy(dst->protocol, "poll_log", sizeof(dst->protocol));
		ESP_LOGW(TAG, "protocol auto_pid is retired on this fork; running poll_log");
	}
	ESP_LOGI(TAG, "dst->protocol: %s", dst->protocol);

	key = cJSON_GetObjectItem(root,"ble_pass");
	if(key == 0)
	{
		goto config_error;
	}
	if(key->valuestring == NULL || strlen(key->valuestring) < 4 || strlen(key->valuestring) > 16)
	{
		goto config_error;
	}
	strlcpy(dst->ble_pass, key->valuestring, sizeof(dst->ble_pass));
	CONFIG_LOG_SECRET("dst->ble_pass", dst->ble_pass);

	key = cJSON_GetObjectItem(root,"ble_power");
	if(key && key->valuestring) {
		// Accept only expected discrete values: -12,-9,-6,-3,0,3,6,9
		int p = atoi(key->valuestring);
		switch(p){
			case -12: case -9: case -6: case -3: case 0: case 3: case 6: case 9:
				strlcpy(dst->ble_power, key->valuestring, sizeof(dst->ble_power));
				break;
			default:
				strlcpy(dst->ble_power, "9", sizeof(dst->ble_power));
				ESP_LOGW(TAG, "Invalid ble_power %d, defaulting to 9", p);
		}
	} else {
		strlcpy(dst->ble_power, "9", sizeof(dst->ble_power));
		ESP_LOGW(TAG, "ble_power missing, defaulting to 9");
	}

	key = cJSON_GetObjectItem(root,"sleep_status");
	if(key == 0)
	{
		goto config_error;
	}

	if (key->valuestring == NULL) {
		goto config_error;
	}
	strlcpy(dst->sleep_status, key->valuestring, sizeof(dst->sleep_status));
	ESP_LOGI(TAG, "dst->sleep_status: %s", dst->sleep_status);

	key = cJSON_GetObjectItem(root,"ble_status");
	if(key == 0)
	{
		goto config_error;
	}

	if (key->valuestring == NULL) {
		goto config_error;
	}
	strlcpy(dst->ble_status, key->valuestring, sizeof(dst->ble_status));
	ESP_LOGI(TAG, "dst->ble_status: %s", dst->ble_status);

	key = cJSON_GetObjectItem(root,"sleep_volt");
	if(key == 0)
	{
		goto config_error;
	}

	if (key->valuestring == NULL) {
		goto config_error;
	}
	strlcpy(dst->sleep_volt, key->valuestring, sizeof(dst->sleep_volt));
	ESP_LOGI(TAG, "dst->sleep_volt: %s", dst->sleep_volt);

	// Task #6: engine-running gate threshold (CSV logger only; separate from sleep_volt).
	// MIGRATION-SAFE: default on a missing/garbage key -- do NOT goto config_error like the
	// sleep_volt block above, or the first boot after upgrade (old NVS has no engine_volt)
	// would wipe the whole config. Range floor 13.0 keeps the ON edge above a healthy NC
	// resting battery (~12.6-12.8 V) even at the slider minimum, so the parked-reads-ON bug
	// cannot return; ceiling 15.0.
	key = cJSON_GetObjectItem(root,"engine_volt");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->engine_volt, VEHICLE_ENGINE_ON_VOLT_DEFAULT_STR, sizeof(dst->engine_volt));
	}
	else
	{
		strlcpy(dst->engine_volt, key->valuestring, sizeof(dst->engine_volt));
		char *ev_end;
		float ev = strtof(dst->engine_volt, &ev_end);
		if(*ev_end != '\0' || ev_end == dst->engine_volt || ev < 13.0f || ev > 15.0f)
		{
			strlcpy(dst->engine_volt, VEHICLE_ENGINE_ON_VOLT_DEFAULT_STR, sizeof(dst->engine_volt));
		}
	}
	ESP_LOGI(TAG, "dst->engine_volt: %s", dst->engine_volt);

	//*****
	// key = cJSON_GetObjectItem(root,"batt_alert");
	// if(key == 0 || (strlen(key->valuestring) > sizeof(dst->batt_alert)))
	// {
	// 	goto config_error;
	// }

	strlcpy(dst->batt_alert, "disable", sizeof(dst->batt_alert));
	ESP_LOGI(TAG, "dst->batt_alert: %s", dst->batt_alert);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"batt_alert_ssid");
	if(key == 0 || key->valuestring == NULL || (strlen(key->valuestring) > sizeof(dst->batt_alert_ssid)))
	{
		goto config_error;
	}

	strlcpy(dst->batt_alert_ssid, key->valuestring, sizeof(dst->batt_alert_ssid));
	ESP_LOGI(TAG, "dst->batt_alert_ssid: %s", dst->batt_alert_ssid);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"batt_alert_pass");
	if(key == 0 || key->valuestring == NULL || (strlen(key->valuestring) > sizeof(dst->batt_alert_pass)))
	{
		goto config_error;
	}

	strlcpy(dst->batt_alert_pass, key->valuestring, sizeof(dst->batt_alert_pass));
	CONFIG_LOG_SECRET("dst->batt_alert_pass", dst->batt_alert_pass);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"batt_alert_volt");
	if(key == 0 || key->valuestring == NULL || (strlen(key->valuestring) > sizeof(dst->batt_alert_volt)))
	{
		goto config_error;
	}

	strlcpy(dst->batt_alert_volt, key->valuestring, sizeof(dst->batt_alert_volt));
	ESP_LOGI(TAG, "dst->batt_alert_volt: %s", dst->batt_alert_volt);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"batt_alert_protocol");
	if(key == 0 || key->valuestring == NULL || (strlen(key->valuestring) > sizeof(dst->batt_alert_protocol)))
	{
		goto config_error;
	}

	strlcpy(dst->batt_alert_protocol, key->valuestring, sizeof(dst->batt_alert_protocol));
	ESP_LOGI(TAG, "dst->batt_alert_protocol: %s", dst->batt_alert_protocol);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"batt_alert_url");
	if(key == 0 || key->valuestring == NULL || (strlen(key->valuestring) > sizeof(dst->batt_alert_url)))
	{
		goto config_error;
	}

	strlcpy(dst->batt_alert_url, key->valuestring, sizeof(dst->batt_alert_url));
	ESP_LOGI(TAG, "dst->batt_alert_url: %s", dst->batt_alert_url);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"batt_alert_port");
	if(key == 0 || key->valuestring == NULL || (strlen(key->valuestring) > sizeof(dst->batt_alert_port)))
	{
		goto config_error;
	}

	strlcpy(dst->batt_alert_port, key->valuestring, sizeof(dst->batt_alert_port));
	ESP_LOGI(TAG, "dst->batt_alert_port: %s", dst->batt_alert_port);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"batt_alert_topic");
	if(key == 0 || key->valuestring == NULL || (strlen(key->valuestring) > sizeof(dst->batt_alert_topic)))
	{
		goto config_error;
	}

	strlcpy(dst->batt_alert_topic, key->valuestring, sizeof(dst->batt_alert_topic));
	ESP_LOGI(TAG, "dst->batt_alert_topic: %s", dst->batt_alert_topic);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"batt_mqtt_user");
	if(key == 0 || key->valuestring == NULL || (strlen(key->valuestring) > sizeof(dst->batt_mqtt_user)))
	{
		goto config_error;
	}

	strlcpy(dst->batt_mqtt_user, key->valuestring, sizeof(dst->batt_mqtt_user));
	ESP_LOGI(TAG, "dst->batt_mqtt_user: %s", dst->batt_mqtt_user);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"batt_mqtt_pass");
	if(key == 0 || key->valuestring == NULL || (strlen(key->valuestring) > sizeof(dst->batt_mqtt_pass)))
	{
		goto config_error;
	}

	strlcpy(dst->batt_mqtt_pass, key->valuestring, sizeof(dst->batt_mqtt_pass));
	CONFIG_LOG_SECRET("dst->batt_mqtt_pass", dst->batt_mqtt_pass);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"batt_alert_time");
	if(key == 0 || key->valuestring == NULL || (strlen(key->valuestring) > sizeof(dst->batt_alert_time)))
	{
		goto config_error;
	}

	strlcpy(dst->batt_alert_time, key->valuestring, sizeof(dst->batt_alert_time));
	ESP_LOGI(TAG, "dst->batt_alert_time: %s", dst->batt_alert_time);
	//*****



	// mqtt_security
	//*****

	//*****
	//***** Optional keys (issue #68): an absent key falls back to its default, and so
	//      does a key whose value is not a JSON string. cJSON only fills valuestring
	//      for string items, so a number/bool/null/array leaves it NULL and copying it
	//      panics the httpd task. A wrong type is treated as absent rather than as an
	//      error, because this same parse runs on the stored config at boot -- rejecting
	//      there would factory-restore the device (the failure mode fixed in #44).
	key = cJSON_GetObjectItem(root,"wakeup_volt");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->wakeup_volt, "13.5", sizeof(dst->wakeup_volt));
	}
	else
	{
		strlcpy(dst->wakeup_volt, key->valuestring, sizeof(dst->wakeup_volt));
	}

	ESP_LOGI(TAG, "dst->wakeup_volt: %s", dst->wakeup_volt);
	//*****
	
	//*****
	key = cJSON_GetObjectItem(root,"sleep_time");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->sleep_time, "5", sizeof(dst->sleep_time));
	}
	else
	{
		/* No range check here on purpose. The 1..30 validation lives in
		 * config_server_get_sleep_time(), which rejects out-of-range values and lets both
		 * callers fall back to a safe default (main/obd.c, main/sleep_mode.c). Validating
		 * at parse time would have to write a default IN PLACE, never reject -- this same
		 * parse runs on the stored config at boot and a rejection factory-restores the
		 * device (issue #44). */
		strlcpy(dst->sleep_time, key->valuestring, sizeof(dst->sleep_time));
	}

	ESP_LOGI(TAG, "dst->sleep_time: %s", dst->sleep_time);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"sta_security");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->sta_security, "wpa3", sizeof(dst->sta_security));
	}
	else
	{
		strlcpy(dst->sta_security, key->valuestring, sizeof(dst->sta_security));
	}

	ESP_LOGI(TAG, "dst->sta_security: %s", dst->sta_security);
	//*****

	//***** Parse optional fallback STA networks *****
	cJSON *fallbacks = cJSON_GetObjectItem(root, "sta_fallbacks");
	if (fallbacks && cJSON_IsArray(fallbacks))
	{
		int count = cJSON_GetArraySize(fallbacks);
		int kept = 0;
		for (int i = 0; i < count && kept < 5; ++i)
		{
			cJSON *item = cJSON_GetArrayItem(fallbacks, i);
			if (!cJSON_IsObject(item))
				continue;
			cJSON *f_ssid = cJSON_GetObjectItem(item, "ssid");
			cJSON *f_pass = cJSON_GetObjectItem(item, "pass");
			cJSON *f_sec  = cJSON_GetObjectItem(item, "security");
			if (!f_ssid || !cJSON_IsString(f_ssid) || strlen(f_ssid->valuestring) == 0 || strlen(f_ssid->valuestring) > 64)
				continue;
			// Password can be empty for open networks; limit to 64
			const char *pass_val = (f_pass && cJSON_IsString(f_pass)) ? f_pass->valuestring : "";
			if (strlen(pass_val) > 64)
				continue;
			const char *sec_val = (f_sec && cJSON_IsString(f_sec)) ? f_sec->valuestring : "wpa3";

			strlcpy(dst->sta_fallbacks[kept].ssid, f_ssid->valuestring, sizeof(dst->sta_fallbacks[kept].ssid));
			strlcpy(dst->sta_fallbacks[kept].pass, pass_val, sizeof(dst->sta_fallbacks[kept].pass));
			if (strcmp(sec_val, "wpa2") == 0 || strcmp(sec_val, "wpa3") == 0)
			{
				strlcpy(dst->sta_fallbacks[kept].security, sec_val, sizeof(dst->sta_fallbacks[kept].security));
			}
			else
			{
				strlcpy(dst->sta_fallbacks[kept].security, "wpa3", sizeof(dst->sta_fallbacks[kept].security));
			}
			kept++;
		}
		dst->sta_fallbacks_count = kept;
		ESP_LOGI(TAG, "Loaded %d STA fallback networks", dst->sta_fallbacks_count);
	}
	else
	{
		ESP_LOGI(TAG, "No STA fallback networks defined");
	}

	//**** SmartConnect fields ****
	key = cJSON_GetObjectItem(root,"home_ssid");
	if(key == 0 || key->valuestring == NULL || strlen(key->valuestring) == 0 || strlen(key->valuestring) > sizeof(dst->home_ssid) - 1)
	{
		strlcpy(dst->home_ssid, "MeatPi", sizeof(dst->home_ssid));
	}
	else
	{
		strlcpy(dst->home_ssid, key->valuestring, sizeof(dst->home_ssid));
	}
	ESP_LOGI(TAG, "dst->home_ssid: %s", dst->home_ssid);

	key = cJSON_GetObjectItem(root,"home_password");
	if(key == 0 || key->valuestring == NULL || strlen(key->valuestring) < 8 || strlen(key->valuestring) > sizeof(dst->home_password) - 1)
	{
		strlcpy(dst->home_password, "TomatoSauce", sizeof(dst->home_password));
	}
	else
	{
		strlcpy(dst->home_password, key->valuestring, sizeof(dst->home_password));
	}
	CONFIG_LOG_SECRET("dst->home_password", dst->home_password);

	key = cJSON_GetObjectItem(root,"home_security");
	if(key == 0 || key->valuestring == NULL || strlen(key->valuestring) == 0 || strlen(key->valuestring) > sizeof(dst->home_security) - 1)
	{
		strlcpy(dst->home_security, "wpa3", sizeof(dst->home_security));
	}
	else
	{
		strlcpy(dst->home_security, key->valuestring, sizeof(dst->home_security));
	}
	ESP_LOGI(TAG, "dst->home_security: %s", dst->home_security);

	key = cJSON_GetObjectItem(root,"home_protocol");
	if(key == 0 || key->valuestring == NULL || strlen(key->valuestring) == 0 || strlen(key->valuestring) > sizeof(dst->home_protocol) - 1)
	{
		strlcpy(dst->home_protocol, "elm327", sizeof(dst->home_protocol));
	}
	else
	{
		strlcpy(dst->home_protocol, key->valuestring, sizeof(dst->home_protocol));
	}
	// A stored "auto_pid" needs no coercion here: config_server_get_home_protocol() has no
	// auto_pid arm, so it already resolves to ELM327 and the SmartConnect override in main.c
	// can never see AUTO_PID. The fallback above is "elm327" only so an absent key and a
	// legacy stored value end up saying the same thing.
	ESP_LOGI(TAG, "dst->home_protocol: %s", dst->home_protocol);

	key = cJSON_GetObjectItem(root,"drive_ssid");
	if(key == 0 || key->valuestring == NULL || strlen(key->valuestring) == 0 || strlen(key->valuestring) > sizeof(dst->drive_ssid) - 1)
	{
		strlcpy(dst->drive_ssid, "MeatPi", sizeof(dst->drive_ssid));
	}
	else
	{
		strlcpy(dst->drive_ssid, key->valuestring, sizeof(dst->drive_ssid));
	}
	ESP_LOGI(TAG, "dst->drive_ssid: %s", dst->drive_ssid);

	key = cJSON_GetObjectItem(root,"drive_password");
	if(key == 0 || key->valuestring == NULL || strlen(key->valuestring) < 8 || strlen(key->valuestring) > sizeof(dst->drive_password) - 1)
	{
		strlcpy(dst->drive_password, "TomatoSauce", sizeof(dst->drive_password));
	}
	else
	{
		strlcpy(dst->drive_password, key->valuestring, sizeof(dst->drive_password));
	}
	CONFIG_LOG_SECRET("dst->drive_password", dst->drive_password);

	key = cJSON_GetObjectItem(root,"drive_security");
	if(key == 0 || key->valuestring == NULL || strlen(key->valuestring) == 0 || strlen(key->valuestring) > sizeof(dst->drive_security) - 1)
	{
		strlcpy(dst->drive_security, "wpa3", sizeof(dst->drive_security));
	}
	else
	{
		strlcpy(dst->drive_security, key->valuestring, sizeof(dst->drive_security));
	}
	ESP_LOGI(TAG, "dst->drive_security: %s", dst->drive_security);

	key = cJSON_GetObjectItem(root,"drive_connection_type");
	if(key == 0 || key->valuestring == NULL || strlen(key->valuestring) == 0 || strlen(key->valuestring) > sizeof(dst->drive_connection_type) - 1)
	{
		strlcpy(dst->drive_connection_type, "wifi", sizeof(dst->drive_connection_type));
	}
	else
	{
		strlcpy(dst->drive_connection_type, key->valuestring, sizeof(dst->drive_connection_type));
	}
	ESP_LOGI(TAG, "dst->drive_connection_type: %s", dst->drive_connection_type);

	key = cJSON_GetObjectItem(root,"drive_mode_timeout");
	if(key == 0 || key->valuestring == NULL || strlen(key->valuestring) == 0 || strlen(key->valuestring) > sizeof(dst->drive_mode_timeout) - 1)
	{
		strlcpy(dst->drive_mode_timeout, "60", sizeof(dst->drive_mode_timeout));
	}
	else
	{
		strlcpy(dst->drive_mode_timeout, key->valuestring, sizeof(dst->drive_mode_timeout));
	}
	ESP_LOGI(TAG, "dst->drive_mode_timeout: %s", dst->drive_mode_timeout);

	key = cJSON_GetObjectItem(root,"drive_protocol");
	if(key == 0 || key->valuestring == NULL || strlen(key->valuestring) == 0 || strlen(key->valuestring) > sizeof(dst->drive_protocol) - 1)
	{
		strlcpy(dst->drive_protocol, "elm327", sizeof(dst->drive_protocol));
	}
	else
	{
		strlcpy(dst->drive_protocol, key->valuestring, sizeof(dst->drive_protocol));
	}
	// See the home_protocol note above: the getter, not the parser, retires auto_pid here.
	ESP_LOGI(TAG, "dst->drive_protocol: %s", dst->drive_protocol);

	//**** End SmartConnect fields ****

	//*****
	key = cJSON_GetObjectItem(root,"csv_log");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->csv_log, "disable", sizeof(dst->csv_log));
	}
	else
	{
		strlcpy(dst->csv_log, key->valuestring, sizeof(dst->csv_log));
	}
	ESP_LOGI(TAG, "dst->csv_log: %s", dst->csv_log);
	//*****

	//*****
	// Coerce any non-enable/disable csv_log value to "disable" so a garbage NVS
	// value can never enable the logger.
	if(strcmp(dst->csv_log, "enable") != 0 && strcmp(dst->csv_log, "disable") != 0)
	{
		strlcpy(dst->csv_log, "disable", sizeof(dst->csv_log));
	}
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"log_filesystem");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->log_filesystem, "littlefs", sizeof(dst->log_filesystem));
	}
	else
	{
		strlcpy(dst->log_filesystem, key->valuestring, sizeof(dst->log_filesystem));
	}
	ESP_LOGI(TAG, "dst->log_filesystem: %s", dst->log_filesystem);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"log_storage");

	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->log_storage, "sdcard", sizeof(dst->log_storage));
	}
	else
	{
		strlcpy(dst->log_storage, key->valuestring, sizeof(dst->log_storage));
	}

	ESP_LOGI(TAG, "dst->log_storage: %s", dst->log_storage);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"log_period");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->log_period, "10", sizeof(dst->log_period));
	}
	else
	{
		/* No range check here, same reasoning as sleep_time above: config_server_get_log_period()
		 * owns the 1..300 validation and main.c falls back to 60 on failure. */
		strlcpy(dst->log_period, key->valuestring, sizeof(dst->log_period));
	}
	ESP_LOGI(TAG, "dst->log_period: %s", dst->log_period);
	//*****

	//***** Activity-LED blink toggle: anything not "enable"/"disable" coerces to
	//      "enable" (default ON). An absent key (old NVS / first upgrade boot,
	//      including the retired numeric led_blink_ms) also defaults ON.
	key = cJSON_GetObjectItem(root,"led_blink");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->led_blink, "enable", sizeof(dst->led_blink));
	}
	else
	{
		strlcpy(dst->led_blink, key->valuestring, sizeof(dst->led_blink));
	}
	if(strcmp(dst->led_blink, "enable") != 0 && strcmp(dst->led_blink, "disable") != 0)
	{
		strlcpy(dst->led_blink, "enable", sizeof(dst->led_blink));
	}
	ESP_LOGI(TAG, "dst->led_blink: %s", dst->led_blink);
	//*****

	//***** Wide CSV (Task #11): csv_grid_hz. Garbage coerces to a safe default so a bad NVS
	//      value can never select an invalid rate. (csv_format removed: Task #16; csv_grid_mode
	//      removed: issue #53 -- the grid is always fixed-rate, a stored key is ignored and
	//      disappears from config.json on the first Submit after the update.)
	key = cJSON_GetObjectItem(root,"csv_grid_hz");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->csv_grid_hz, "auto", sizeof(dst->csv_grid_hz));
	}
	else
	{
		strlcpy(dst->csv_grid_hz, key->valuestring, sizeof(dst->csv_grid_hz));
		//***** "auto" (issue #23): grid tracks the measured poll sweep rate; otherwise 1-100 Hz.
		if(strcmp(dst->csv_grid_hz, "auto") != 0)
		{
			char *gh_end;
			long gh = strtol(dst->csv_grid_hz, &gh_end, 10);
			if(*gh_end != '\0' || gh_end == dst->csv_grid_hz || gh < 1 || gh > (long)WICAN_LOG_MAX_HZ)
			{
				strlcpy(dst->csv_grid_hz, "auto", sizeof(dst->csv_grid_hz));
			}
		}
	}
	ESP_LOGI(TAG, "dst->csv_grid_hz: %s", dst->csv_grid_hz);
	//*****

	//***** Engine-running CSV gate (Stage 1): anything not "enable"/"disable" coerces to "enable"
	//      (default ON). An absent key (old NVS / first upgrade boot) also defaults ON.
	key = cJSON_GetObjectItem(root,"csv_require_engine");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->csv_require_engine, "enable", sizeof(dst->csv_require_engine));
	}
	else
	{
		strlcpy(dst->csv_require_engine, key->valuestring, sizeof(dst->csv_require_engine));
	}
	if(strcmp(dst->csv_require_engine, "enable") != 0 && strcmp(dst->csv_require_engine, "disable") != 0)
	{
		strlcpy(dst->csv_require_engine, "enable", sizeof(dst->csv_require_engine));
	}
	ESP_LOGI(TAG, "dst->csv_require_engine: %s", dst->csv_require_engine);
	//*****

	key = cJSON_GetObjectItem(root,"ap_auto_disable");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->ap_auto_disable, "disable", sizeof(dst->ap_auto_disable));
	}
	else
	{
		strlcpy(dst->ap_auto_disable, key->valuestring, sizeof(dst->ap_auto_disable));
	}

	ESP_LOGI(TAG, "dst->ap_auto_disable: %s", dst->ap_auto_disable);

	//*****
	// Periodic wakeup is retired: the wake was only an esp_restart() that nothing
	// distinguishes from a cold boot (nobody reads POWER_WAKE), and this build has no
	// outbound client to report with -- so it just burned parked battery. Pinned here
	// rather than in the UI alone so already-deployed devices with periodic_wakeup=enable
	// stop waking at their next boot, and so a direct POST /store_config cannot re-arm it.
	// Same treatment as batt_alert above. To revive for issue #24, restore the parse.
	// key = cJSON_GetObjectItem(root,"periodic_wakeup");
	// if(key == 0)
	// {
	// 	strlcpy(dst->periodic_wakeup, "disable", sizeof(dst->periodic_wakeup));
	// }
	// else
	// {
	// 	strlcpy(dst->periodic_wakeup, key->valuestring, sizeof(dst->periodic_wakeup));
	// }

	strlcpy(dst->periodic_wakeup, "disable", sizeof(dst->periodic_wakeup));
	ESP_LOGI(TAG, "dst->periodic_wakeup: %s", dst->periodic_wakeup);
	//*****

	//*****	
	key = cJSON_GetObjectItem(root,"wakeup_interval");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->wakeup_interval, "60", sizeof(dst->wakeup_interval));
	}
	else
	{
		strlcpy(dst->wakeup_interval, key->valuestring, sizeof(dst->wakeup_interval));
	}

	ESP_LOGI(TAG, "dst->wakeup_interval: %s", dst->wakeup_interval);
	//*****	


	//*****
	// sleep_disable_agree
	/* #4 wake-on-CAN. Missing key -> "enable": every device provisioned before this key
	 * existed must resolve to a valid value, because this same parse runs on the STORED
	 * config at boot and a rejection factory-restores the device (issue #44). */
	key = cJSON_GetObjectItem(root,"can_wake");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->can_wake, "enable", sizeof(dst->can_wake));
	}
	else
	{
		strlcpy(dst->can_wake, key->valuestring, sizeof(dst->can_wake));
	}
	ESP_LOGI(TAG, "dst->can_wake: %s", dst->can_wake);
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"sleep_disable_agree");
	if(key == 0 || key->valuestring == NULL)
	{
		strlcpy(dst->sleep_disable_agree, "no", sizeof(dst->sleep_disable_agree));
	}
	else
	{
		strlcpy(dst->sleep_disable_agree, key->valuestring, sizeof(dst->sleep_disable_agree));
	}

	ESP_LOGI(TAG, "dst->sleep_disable_agree: %s", dst->sleep_disable_agree);
	//*****	

	//*****
	// imu_threshold
	key = cJSON_GetObjectItem(root,"imu_threshold");
	if(key == 0 || key->valuestring == NULL)
	{
		ESP_LOGI(TAG, "imu_threshold not found, loading default");
		strcpy(dst->imu_threshold, "8");
	}
	else
	{
		if(strlen(key->valuestring) > 0 && strlen(key->valuestring) < 16)
		{
			strcpy(dst->imu_threshold, key->valuestring);
		}
		else
		{
			ESP_LOGI(TAG, "imu_threshold invalid length, loading default");
			strcpy(dst->imu_threshold, "8");
		}
	}

	ESP_LOGI(TAG, "dst->imu_threshold: %s", dst->imu_threshold);
	//*****

	//*****
	//*****

	//*****
	key = cJSON_GetObjectItem(root,"debug");
	if(key == 0 || key->valuestring == NULL || (strlen(key->valuestring) == 0))
	{
		dst->debug_enabled = 0; // default to no debug
	}
	else
	{
		strcmp(key->valuestring, "enabled") == 0 ? (dst->debug_enabled = 1) : (dst->debug_enabled = 0);
	}

	if(dst->debug_enabled)
	{
		ESP_LOGW(TAG, "\r\n\r\n*****************Debug mode enabled*****************\r\n\r\n");
	}
	else
	{
		ESP_LOGI(TAG, "Debug mode disabled");
	}
	//*****

	cJSON_Delete(root);
	return true;


config_error:
	cJSON_Delete(root);
	return false;

config_error_no_json:
	return false;
}

// Boot entry: parse config.json into the live device_config. On failure, restore
// the factory-default config and reboot (the old in-line recovery, hoisted out so
// the parser itself is side-effect-free and reusable for the live-apply path).
static void config_server_load_cfg(char *cfg)
{
	if (config_server_parse_cfg_into(&device_config, cfg))
	{
		return;
	}
	ESP_LOGE(TAG, "config parse failed, restoring default config");
	unlink(FS_MOUNT_POINT"/config.json");
	FILE* f = fopen(FS_MOUNT_POINT"/config.json", "w");
	if (f) {
		fputs(device_config_default, f);
		fclose(f);
	}
	vTaskDelay(3000 / portTICK_PERIOD_MS);
	restart_tracker_restart(RESTART_TRACKER_PLANNED_REASON_CONFIG_RECOVERY,
					 RESTART_TRACKER_SOURCE_CONFIG_SERVER,
					 RESTART_TRACKER_FLAG_SETTINGS_SAVED | RESTART_TRACKER_FLAG_RECOVERY_ACTION);
}

void config_server_wifi_connected(bool flag)
{
	if(flag)
	{
		xEventGroupSetBits( xServerEventGroup, WIFI_CONNECTED_BIT );
	}
	else
	{
		xEventGroupClearBits( xServerEventGroup, WIFI_CONNECTED_BIT );
	}
}
//
//bool config_server_get_wifi_connected(void)
//{
//	EventBits_t uxBits;
//	if(xServerEventGroup != NULL)
//	{
//		uxBits = xEventGroupGetBits(xServerEventGroup);
//
//		return (uxBits & WIFI_CONNECTED_BIT)?1:0;
//	}
//	else return 0;
//}

void config_server_set_sta_ip(char* ip)
{
	xQueueOverwrite(xip_Queue, ip);
}
void config_server_get_sta_ip(char* ip)
{
	xQueuePeek(xip_Queue, ip, 0);
}

void vrestartTimerCallback( TimerHandle_t xTimer )
{
//	vTaskDelay(1000 / portTICK_PERIOD_MS);
	// NOTE: the reboot is recorded by the event log AFTER the fact, on the next boot (the boot event
	// reads restart_tracker's planned-reason), so nothing new touches this reset chokepoint. Events
	// emitted before a reboot (OTA_OK, etc.) are already fsync'd by the writer within ~1s of emission.
	restart_tracker_restart(s_reboot_reason, s_reboot_source, s_reboot_flags);
}

static void register_server_uris(void)
{
	ESP_LOGI(TAG, "Registering URI handlers");
	httpd_register_uri_handler(server, &index_uri);
	httpd_register_uri_handler(server, &store_config_uri);
	httpd_register_uri_handler(server, &check_status_uri);
	httpd_register_uri_handler(server, &load_config_uri);
	httpd_register_uri_handler(server, &logo_uri);
	httpd_register_uri_handler(server, &file_upload);
	httpd_register_uri_handler(server, &system_reboot);
	httpd_register_uri_handler(server, &store_auto_data_uri);
	httpd_register_uri_handler(server, &load_pid_auto_uri);
	httpd_register_uri_handler(server, &upload_sd_uri);
	httpd_register_uri_handler(server, &system_commands);
	httpd_register_uri_handler(server, &scan_available_pids_uri);
	httpd_register_uri_handler(server, &std_pid_info);
	httpd_register_uri_handler(server, &poll_status_uri);
	httpd_register_uri_handler(server, &wake_probe_uri);  /* #4 -- permanent sleep/resume diagnostic */
	httpd_register_uri_handler(server, &sleep_status_uri);  /* #85 -- feeds the countdown banner */

	//Add before this line
	httpd_register_uri_handler(server, &csv_status_uri);
	httpd_register_uri_handler(server, &csv_list_uri);
	httpd_register_uri_handler(server, &csv_download_uri);
	httpd_register_uri_handler(server, &csv_control_uri);
	httpd_register_uri_handler(server, &datalog_control_uri);   /* POST /datalog?op=pause|resume (task #36.C) */
	httpd_register_uri_handler(server, &datalog_status_uri);    /* GET  /datalog -> live coexistence state */
	httpd_register_uri_handler(server, &sd_files_get_uri);
	httpd_register_uri_handler(server, &sd_files_post_uri);
	event_log_register_handlers(server);   // GET /event_log* (Task #24) -- before the catch-all wildcard
}

//static char* device_config = NULL;
static uint8_t esp_fatfs_flag = 0;
static httpd_config_t config = HTTPD_DEFAULT_CONFIG();
static httpd_handle_t config_server_init(void)
{
//	const char* base_path = "/"; //useless?
//
//    if (server_data)
//    {
//        ESP_LOGE(TAG, "File server already started");
//        return ESP_ERR_INVALID_STATE;
//    }
//
//    /* Allocate memory for server data */
//    server_data = calloc(1, sizeof(struct file_server_data));
//    if (!server_data)
//    {
//        ESP_LOGE(TAG, "Failed to allocate memory for server data");
//        return ESP_ERR_NO_MEM;
//    }
//
//    strlcpy(server_data->base_path, base_path,
//            sizeof(server_data->base_path));

    config.lru_purge_enable = true;
	config.uri_match_fn = httpd_uri_match_wildcard;
    if(xServerEventGroup == NULL)
    {
		server_data.scratch = heap_caps_malloc(SCRATCH_BUFSIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if(server_data.scratch == NULL)
		{
			ESP_LOGE(TAG, "Failed to allocate memory for server data");
			return NULL;
		}
		memset(server_data.scratch, 0, SCRATCH_BUFSIZE);
		
		xServerEventGroup = xEventGroupCreateStatic(&server_event_group_buffer);
    	config_server_wifi_connected(0);
    }

    if(xip_Queue == NULL)
    {
		xip_Queue = xQueueCreateStatic(1, 20, xip_queue_storage, &xip_queue_struct);
    }

	

	if(esp_fatfs_flag == 0) 
	{
		filesystem_init();
		// Handle config.json
		FILE* f = fopen(FS_MOUNT_POINT"/config.json", "r");
		if (f == NULL)
		{
			ESP_LOGI(TAG, "Config file does not exist, loading default");
			f = fopen(FS_MOUNT_POINT"/config.json", "w");
			if (f != NULL)
			{
				fputs(device_config_default, f);
				fclose(f);
				f = fopen(FS_MOUNT_POINT"/config.json", "r");
				ESP_LOGW(TAG, "Config file trying to load again");
			}
		}

		if (f != NULL)
		{
			fseek(f, 0, SEEK_END);
			long filesize = ftell(f);
			fseek(f, 0, SEEK_SET);

			device_config_file = heap_caps_malloc(filesize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
			if (device_config_file != NULL)
			{
				memset(device_config_file, 0, filesize + 1);
				fread(device_config_file, sizeof(char), filesize, f);
				device_config_file[filesize] = 0;
				// Size only, never the body: this file holds every stored
				// credential in plaintext, and it used to be dumped whole at
				// each boot. The per-field logging further down reports each
				// key's shape without its value.
				ESP_LOGI(TAG, "config.json loaded: %ld bytes", filesize);
				fclose(f);	//close file after reading, config_server_load_cfg might unlink it
				config_server_load_cfg(device_config_file);
			}
			else
			{
				ESP_LOGE(TAG, "Failed to allocate memory for config file");
				fclose(f);
			}
			
		}

	}

    xrestartTimer= xTimerCreate
                       ( /* Just a text name, not used by the RTOS
                         kernel. */
                         "Timer",
                         /* The timer period in ticks, must be
                         greater than 0. */
						 (2000 / portTICK_PERIOD_MS),
                         /* The timers will auto-reload themselves
                         when they expire. */
                         pdTRUE,
                         /* The ID is used to store a count of the
                         number of times the timer has expired, which
                         is initialised to 0. */
                         ( void * ) 0,
                         /* Each timer calls the same callback when
                         it expires. */
                         vrestartTimerCallback
                       );

	// Start the httpd server (reserve extra slots for cert manager endpoints)
	// NOTE: the catch-all "/*" file server (get_uri_common) is registered LAST, so it
	// must fit within this cap. The 3 CSV-logger endpoints pushed the total past 38,
	// which silently dropped "/*" and 404'd every embedded asset (main.js, etc.).
	config.max_uri_handlers = 48;
	config.stack_size = (10*1024);
	config.max_open_sockets = 15;
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
	if (httpd_start(&server, &config) == ESP_OK)
    {
        // Set URI handlers
		register_server_uris();
		// Register certificate manager endpoints (before wildcard catch-all so they match first)
		autopid_register_handlers(server);
		restart_tracker_register_handlers(server);
		// Now register catch-all wildcard
		httpd_register_uri_handler(server, &get_uri_common);
		ESP_LOGI(TAG, "Server started successfully");
		
        #if CONFIG_EXAMPLE_BASIC_AUTH
        httpd_register_basic_auth(server);
        #endif
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}
void config_server_restart(void)
{
    // Start the httpd server
    ESP_LOGI(TAG, "Restarting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
		register_server_uris();
		autopid_register_handlers(server);
		restart_tracker_register_handlers(server);
		httpd_register_uri_handler(server, &get_uri_common);
		ESP_LOGI(TAG, "Server restarted successfully");
        return;
    }

    ESP_LOGI(TAG, "Error starting server!");
}
void config_server_stop(void)
{
    if (server)
    {
        ESP_LOGI(TAG, "Stopping webserver");
        httpd_stop(server);
        server = NULL;
    }
}

// ======= Fallback STA networks getters =======
int config_server_get_sta_fallbacks_count(void)
{
	return device_config.sta_fallbacks_count;
}

const char *config_server_get_sta_fallback_ssid(int index)
{
	if (index < 0 || index >= device_config.sta_fallbacks_count) return NULL;
	return device_config.sta_fallbacks[index].ssid;
}

const char *config_server_get_sta_fallback_pass(int index)
{
	if (index < 0 || index >= device_config.sta_fallbacks_count) return NULL;
	return device_config.sta_fallbacks[index].pass;
}

wifi_security_t config_server_get_sta_fallback_security(int index)
{
	if (index < 0 || index >= device_config.sta_fallbacks_count) return WIFI_MAX;
	const char *sec = device_config.sta_fallbacks[index].security;
	if (strcmp(sec, "open") == 0) return WIFI_OPEN;
	if (strcmp(sec, "wpa2") == 0) return WIFI_WPA2_PSK;
	if (strcmp(sec, "wpa3") == 0) return WIFI_WPA3_PSK;
	return WIFI_WPA2_PSK;
}
void config_server_start(QueueHandle_t *xRXp_Queue, uint8_t connected_led, char * did)
{
    if (server == NULL)
    {
        device_id = did;
        ws_led = connected_led;
        ESP_LOGI(TAG, "Starting webserver");
        server = config_server_init();
    }
}

int8_t config_server_get_ble_config(void)
{
	if(strcmp(device_config.ble_status, "enable") == 0)
	{
		return 1;
	}
	else if(strcmp(device_config.ble_status, "disable") == 0)
	{
		return 0;
	}
	return -1;
}

/* #4: enabled unless EXPLICITLY "disable", so an unset/garbled value degrades to the feature
 * being ON. Every failure mode inside can_wake falls back to today's voltage-only behaviour,
 * so ON is the safe default. */
int8_t config_server_get_can_wake(void)
{
	if(strcmp(device_config.can_wake, "disable") == 0)
	{
		return 0;
	}
	return 1;
}

/* --- Firmware-OTA-in-progress fence -------------------------------------------------------
 * A firmware upload and the sleep teardown are mutually destructive: the teardown calls
 * wifi_mgr_deinit(), and doing that underneath a live HTTP upload pulls the network stack out
 * from under the task still using it. Observed on the bench as a panic at a non-code address
 * (pc=0x20657079) twelve seconds after a sleep entry, with an upload in flight.
 *
 * The #86 ECU-flash interlock did not cover this: FLASH_ACTIVE_BIT means "a flash codec owns the
 * CAN bus", which a firmware update over WiFi never touches. Same shape of problem, different
 * resource, so it gets its own flag and shares the same bounded-postpone logic.
 *
 * Plain volatile bool: written by the httpd task, read by the sleep task, single machine word,
 * and a stale read costs at most one extra retry of a decision that is already retried. */
static volatile bool s_ota_active = false;

void config_server_ota_active_set(bool active)
{
	s_ota_active = active;
}

bool config_server_ota_active(void)
{
	return s_ota_active;
}

int8_t config_server_get_sleep_config(void)
{
	if(strcmp(device_config.sleep_status, "enable") == 0 || strcmp(device_config.sleep_disable_agree, "no") == 0)
	{
		return 1;
	}
	else if(strcmp(device_config.sleep_status, "disable") == 0 && strcmp(device_config.sleep_disable_agree, "yes") == 0)
	{
		return 0;
	}
	return 1;
}

int8_t config_server_get_sleep_volt(float *sleep_volt)
{
	*sleep_volt = atof(device_config.sleep_volt);

	if(*sleep_volt >= 12.0f && *sleep_volt <= 15.0f)
	{
		return 1;
	}
	return -1;
}

// Task #6: dedicated engine-running gate for the CSV logger. Range floor 13.0 keeps the ON
// edge above a resting battery so a parked car can't read "ignition on" (the original bug).
int8_t config_server_get_engine_volt(float *engine_volt)
{
	*engine_volt = atof(device_config.engine_volt);

	if(*engine_volt >= 13.0f && *engine_volt <= 15.0f)
	{
		return 1;
	}
	return -1;
}

int8_t config_server_get_wakeup_volt(float *wakeup_volt)
{
	*wakeup_volt = atof(device_config.wakeup_volt);

	if(*wakeup_volt >= 12.0f && *wakeup_volt <= 15.0f)
	{
		return 1;
	}
	return -1;
}

int8_t config_server_get_sleep_time(uint32_t *sleep_time)
{
    char *endptr;
    long slp_t = strtol(device_config.sleep_time, &endptr, 10);
    
    // Check for conversion errors
    if (*endptr != '\0' || endptr == device_config.sleep_time)
	{
        return -1;
    }
    
    // Validate range
    if (slp_t < 1 || slp_t > 30)
	{
        return -1;
    }
    
    *sleep_time = (uint32_t)slp_t;
    return 1;
}

int8_t config_server_get_periodic_wakeup(void)
{
	if(strcmp(device_config.periodic_wakeup, "enable") == 0)
	{
		return 1;
	}
	return 0;
}

int8_t config_server_get_wakeup_interval(uint32_t *wakeup_interval)
{
    char *endptr;
    long wk_int = strtol(device_config.wakeup_interval, &endptr, 10);
    
    // Check for conversion errors
    if (*endptr != '\0' || endptr == device_config.wakeup_interval)
	{
        return -1;
    }
    
    // Validate range
    if (wk_int < 5 || wk_int > 240)
	{
        return -1;
    }
    
    *wakeup_interval = (uint32_t)wk_int;
    return 1;
}

int8_t config_server_get_battery_alert_config(void)
{
	if(strcmp(device_config.batt_alert, "enable") == 0)
	{
		return 1;
	}
	else if(strcmp(device_config.batt_alert, "disable") == 0)
	{
		return 0;
	}
	return -1;
}

int32_t config_server_get_alert_port(void)
{
	int port_val = atoi(device_config.batt_alert_port);

	if(port_val > 0 && port_val <= 65535)
	{
		return port_val;
	}
	return -1;
}

char *config_server_get_alert_ssid(void)
{
	return device_config.batt_alert_ssid;
}

char *config_server_get_alert_pass(void)
{
	return device_config.batt_alert_pass;
}

char *config_server_get_alert_protocol(void)
{
	return device_config.batt_alert_protocol;
}

char *config_server_get_alert_url(void)
{
	return device_config.batt_alert_url;
}

char *config_server_get_alert_topic(void)
{
	return device_config.batt_alert_topic;
}

char *config_server_get_alert_mqtt_user(void)
{
	return device_config.batt_mqtt_user;
}

char *config_server_get_alert_mqtt_pass(void)
{
	return device_config.batt_mqtt_pass;
}

int config_server_get_alert_time(void)
{
	if(strcmp(device_config.batt_alert_time, "1") == 0)
	{
		return 1;
	}
	else if(strcmp(device_config.batt_alert_time, "6") == 0)
	{
		return 6;
	}
	else if(strcmp(device_config.batt_alert_time, "12") == 0)
	{
		return 12;
	}
	else if(strcmp(device_config.batt_alert_time, "24") == 0)
	{
		return 24;
	}
	else
	{
		return -1;
	}

}

int8_t config_server_get_alert_volt(float *alert_volt)
{
	*alert_volt = atof(device_config.batt_alert_volt);

	if(device_config.batt_alert_volt[2] != '.')
	{
		return -1;
	}

	if(*alert_volt > 8.0f && *alert_volt <= 15.0f)
	{
		return 1;
	}
	return -1;
}
















wifi_security_t config_server_get_sta_security(void)
{
	if (strcmp(device_config.sta_security, "wpa2") == 0)
	{
		return WIFI_WPA2_PSK;
	}
	else if (strcmp(device_config.sta_security, "wpa3") == 0)
	{
		return WIFI_WPA3_PSK;
	}
	return WIFI_MAX;
}

int8_t config_server_get_csv_log(void)
{
	if(strcmp(device_config.csv_log, "enable") == 0)
	{
		return 1;
	}
	else if(strcmp(device_config.csv_log, "disable") == 0)
	{
		return 0;
	}

	return -1;
}

int8_t config_server_get_led_blink_enabled(void)
{
	// Default ON: "disable" holds a solid activity color; "enable" or any other
	// value blinks. Normalized to enable/disable at load time.
	return (strcmp(device_config.led_blink, "disable") == 0) ? 0 : 1;
}

int8_t config_server_get_ap_auto_disable(void)
{
	if(strcmp(device_config.ap_auto_disable, "enable") == 0)
	{
		return 1;
	}
	else if(strcmp(device_config.ap_auto_disable, "disable") == 0)
	{
		return 0;
	}
	return 0;
}

log_filesystem_t config_server_get_log_filesystem(void)
{
	if(strcmp(device_config.log_filesystem, "littlefs") == 0)
	{
		return LOG_FS_LITTLEFS;
	}
	else if(strcmp(device_config.log_filesystem, "fatfs") == 0)
	{
		return LOG_FS_FATFS;
	}
	return MAX_LOG_FS;
}

log_storage_t config_server_get_log_storage(void)
{
	if(strcmp(device_config.log_storage, "sdcard") == 0)
	{
		return LOG_SDCARD;
	}
	else if(strcmp(device_config.log_storage, "internal") == 0)
	{
		return LOG_INTERNAL;
	}
	return MAX_LOG_STORAGE;
}

int8_t config_server_get_log_period(uint32_t *log_period)
{
	char *endptr;
	long log_int = strtol(device_config.log_period, &endptr, 10);
	
	// Check for conversion errors
	if (*endptr != '\0' || endptr == device_config.log_period)
	{
		return -1;
	}
	
	// Validate range
	if (log_int < 1 || log_int > 300)
	{
		return -1;
	}
	
	*log_period = (uint32_t)log_int;
	return 1;
}

int8_t config_server_get_csv_require_engine(void)
{
	// Default ON: "disable" turns the gate off; "enable" or any other value keeps it on.
	if(strcmp(device_config.csv_require_engine, "disable") == 0)
	{
		return 0;
	}
	return 1;
}

int8_t config_server_get_csv_grid_hz(uint32_t *hz)
{
	//***** "auto" (issue #23) reports as hz=0: the CSV writer derives the grid period from
	//      the measured poll sweep rate instead of a fixed value.
	if(strcmp(device_config.csv_grid_hz, "auto") == 0)
	{
		*hz = 0;
		return 1;
	}
	char *endptr;
	long v = strtol(device_config.csv_grid_hz, &endptr, 10);

	if(*endptr != '\0' || endptr == device_config.csv_grid_hz)
	{
		return -1;
	}
	if(v < 1 || v > (long)WICAN_LOG_MAX_HZ)
	{
		return -1;
	}
	*hz = (uint32_t)v;
	return 1;
}

int8_t config_server_get_imu_threshold(uint8_t *imu_threshold)
{
	char *endptr;
	long imu_int = strtol(device_config.imu_threshold, &endptr, 10);
	
	// Check for conversion errors
	if (*endptr != '\0' || endptr == device_config.imu_threshold)
	{
		return -1;
	}
	
	// Validate range (1-32 for ICM-42670-P)
	if (imu_int < 1 || imu_int > 32)
	{
		return -1;
	}
	
	*imu_threshold = (uint8_t)imu_int;
	return 1;
}

bool config_server_is_debug_enabled(void)
{
	return device_config.debug_enabled;
}

int8_t config_server_get_ble_power(int8_t *power_dbm)
{
	if(!power_dbm) return -1;
	int p = atoi(device_config.ble_power);
	switch(p){
		case -12: case -9: case -6: case -3: case 0: case 3: case 6: case 9:
			*power_dbm = (int8_t)p;
			return 0;
		default:
			*power_dbm = 9;
			return -1; // indicate defaulted
	}
}

void config_server_set_ble_config(uint8_t b)
{
	cJSON * root;
	root = cJSON_Parse(device_config_file);
	if(b == 1)
	{
		cJSON_SetValuestring(cJSON_GetObjectItem(root,"ble_status"), "enable");
	}
	else if(b==0)
	{
		cJSON_SetValuestring(cJSON_GetObjectItem(root,"ble_status"), "disable");
	}
	const char *resp_str = cJSON_PrintUnformatted(root);
	ESP_LOGI(TAG, "resp_str:%s", resp_str);
	FILE* f = fopen(FS_MOUNT_POINT"/config.json", "w");
	if (f != NULL)
	{
		fprintf(f, resp_str);
		fclose(f);
	}
	// xTimerStart( xrestartTimer, 0 );
	config_server_schedule_reboot(RESTART_TRACKER_PLANNED_REASON_CONFIG_APPLY,
						 RESTART_TRACKER_SOURCE_CONFIG_SERVER,
						 RESTART_TRACKER_FLAG_SETTINGS_SAVED);
	free((void *)resp_str);
    cJSON_Delete(root);
}

