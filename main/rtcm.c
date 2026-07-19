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

#include "rtcm.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "dev_status.h"

#define TAG "rtcm"

#define RTCM_I2C_TIMEOUT_MS 1000

#define RX8130_ADDR             0x32

#define RX8130_REG_SEC          0x10
#define RX8130_REG_MIN          0x11 
#define RX8130_REG_HOUR         0x12
#define RX8130_REG_CTRL1        0x30
#define RX8130_REG_CTRL2        0x32
#define RX8130_REG_EVT_CTRL     0x1C
#define RX8130_REG_EVT1         0x1D
#define RX8130_REG_EVT2         0x1E
#define RX8130_REG_EVT3         0x1F
#define RX8130_REG_WEEK         0x13
#define RX8130_REG_DAY          0x14
#define RX8130_REG_MONTH        0x15
#define RX8130_REG_YEAR         0x16
#define RX8130_REG_ID           0x17

static i2c_port_t rtcm_i2c = I2C_NUM_MAX;

static esp_err_t rx8130_register_read(uint8_t reg_addr, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(rtcm_i2c, RX8130_ADDR, &reg_addr, 1, data, len, pdMS_TO_TICKS(RTCM_I2C_TIMEOUT_MS));
}

static esp_err_t rx8130_register_write(uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_write_to_device(rtcm_i2c, RX8130_ADDR, write_buf, 2, pdMS_TO_TICKS(RTCM_I2C_TIMEOUT_MS));
}

esp_err_t rtcm_get_time(uint8_t *hour, uint8_t *min, uint8_t *sec)
{
    esp_err_t ret;

    ret = rx8130_register_read(RX8130_REG_SEC, sec, 1);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_read(RX8130_REG_MIN, min, 1);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_read(RX8130_REG_HOUR, hour, 1);
    return ret;
}

esp_err_t rtcm_set_time(uint8_t hour, uint8_t min, uint8_t sec)
{
    esp_err_t ret;

    ret = rx8130_register_write(RX8130_REG_SEC, sec);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_MIN, min);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_HOUR, hour);
    return ret;
}

esp_err_t rtcm_get_date(uint8_t *year, uint8_t *month, uint8_t *day, uint8_t *weekday)
{
    esp_err_t ret;

    ret = rx8130_register_read(RX8130_REG_YEAR, year, 1);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_read(RX8130_REG_MONTH, month, 1);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_read(RX8130_REG_DAY, day, 1);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_read(RX8130_REG_WEEK, weekday, 1);
    return ret;
}

esp_err_t rtcm_set_date(uint8_t year, uint8_t month, uint8_t day, uint8_t weekday)
{
    esp_err_t ret;

    ret = rx8130_register_write(RX8130_REG_YEAR, year);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_MONTH, month);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_DAY, day);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_WEEK, weekday);
    return ret;
}

esp_err_t rtcm_get_device_id(uint8_t *id)
{
    return rx8130_register_read(RX8130_REG_ID, id, 1);
}

esp_err_t rtcm_get_iso8601_time(char *timestamp, size_t max_len)
{
    if (timestamp == NULL || max_len < 20) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Try to get time from RTCM module
    uint8_t hour, min, sec;
    uint8_t year, month, day, weekday;
    
    if (rtcm_get_time(&hour, &min, &sec) == ESP_OK && 
        rtcm_get_date(&year, &month, &day, &weekday) == ESP_OK) {
        
        // Convert BCD format to decimal
        uint8_t hour_dec = ((hour >> 4) & 0x0F) * 10 + (hour & 0x0F);
        uint8_t min_dec = ((min >> 4) & 0x0F) * 10 + (min & 0x0F);
        uint8_t sec_dec = ((sec >> 4) & 0x0F) * 10 + (sec & 0x0F);
        uint8_t year_dec = ((year >> 4) & 0x0F) * 10 + (year & 0x0F);
        uint8_t month_dec = ((month >> 4) & 0x0F) * 10 + (month & 0x0F);
        uint8_t day_dec = ((day >> 4) & 0x0F) * 10 + (day & 0x0F);
        
        // Format timestamp 
        snprintf(timestamp, max_len, "20%02d-%02d-%02dT%02d:%02d:%02d", 
                year_dec, month_dec, day_dec, hour_dec, min_dec, sec_dec);
                
        return ESP_OK;
    } else {
        // Use system time as fallback
        time_t now;
        struct tm timeinfo;
        
        time(&now);
        localtime_r(&now, &timeinfo);
        strftime(timestamp, max_len, "%Y-%m-%dT%H:%M:%S", &timeinfo);
        
        ESP_LOGW(TAG, "RTCM time not available, using system time: %s", timestamp);
        return ESP_OK;
    }
}

/*
 * The RX8130 stores UTC, but mktime() interprets struct tm in the local zone
 * (TZ is local wall-clock since issue #32), so UTC->epoch needs TZ-independent
 * civil-date arithmetic. Exact for the 2000-2100 range the RTC can express.
 */
static time_t rtcm_utc_tm_to_epoch(const struct tm *timeinfo)
{
    int year = timeinfo->tm_year + 1900;
    int month = timeinfo->tm_mon + 1;
    int day = timeinfo->tm_mday;

    year -= month <= 2;
    const int era = year / 400;
    const int yoe = year - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + doe - 719468;

    return (time_t)(days * 86400 + timeinfo->tm_hour * 3600 + timeinfo->tm_min * 60 + timeinfo->tm_sec);
}

time_t rtcm_bcd_to_unix_timestamp(uint8_t hour, uint8_t min, uint8_t sec,
                                 uint8_t year, uint8_t month, uint8_t day)
{
    // Convert BCD format to decimal
    uint8_t hour_dec = ((hour >> 4) & 0x0F) * 10 + (hour & 0x0F);
    uint8_t min_dec = ((min >> 4) & 0x0F) * 10 + (min & 0x0F);
    uint8_t sec_dec = ((sec >> 4) & 0x0F) * 10 + (sec & 0x0F);
    uint8_t year_dec = ((year >> 4) & 0x0F) * 10 + (year & 0x0F);
    uint8_t month_dec = ((month >> 4) & 0x0F) * 10 + (month & 0x0F);
    uint8_t day_dec = ((day >> 4) & 0x0F) * 10 + (day & 0x0F);
    
    struct tm timeinfo;
    timeinfo.tm_year = 100 + year_dec; // Years since 1900 (assuming 20xx)
    timeinfo.tm_mon = month_dec - 1;   // Months are 0-based
    timeinfo.tm_mday = day_dec;
    timeinfo.tm_hour = hour_dec;
    timeinfo.tm_min = min_dec;
    timeinfo.tm_sec = sec_dec;
    timeinfo.tm_isdst = -1;            // Not used
    
    // Validate time components to avoid invalid timestamps
    if (timeinfo.tm_year < 100 || timeinfo.tm_year > 200 ||  // Year from 2000-2100
        timeinfo.tm_mon < 0 || timeinfo.tm_mon > 11 ||       // Month 0-11
        timeinfo.tm_mday < 1 || timeinfo.tm_mday > 31 ||     // Day 1-31
        timeinfo.tm_hour < 0 || timeinfo.tm_hour > 23 ||     // Hour 0-23
        timeinfo.tm_min < 0 || timeinfo.tm_min > 59 ||       // Minute 0-59
        timeinfo.tm_sec < 0 || timeinfo.tm_sec > 59) {       // Second 0-59
        ESP_LOGE(TAG, "Invalid time components: %02d-%02d-%02d %02d:%02d:%02d", 
                 year_dec, month_dec, day_dec, hour_dec, min_dec, sec_dec);
        return 0;
    }
    
    return rtcm_utc_tm_to_epoch(&timeinfo);
}

time_t rtcm_get_unix_timestamp(void)
{
    uint8_t hour, min, sec;
    uint8_t year, month, day, weekday;
    
    // Read current time and date from RTC
    if (rtcm_get_time(&hour, &min, &sec) != ESP_OK || 
        rtcm_get_date(&year, &month, &day, &weekday) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get time/date from RTC");
        return 0;
    }
    
    return rtcm_bcd_to_unix_timestamp(hour, min, sec, year, month, day);
}

static esp_err_t update_rtc_from_system_time(void)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    gmtime_r(&now, &timeinfo); // RX8130 stores UTC; TZ is local wall-clock (issue #32)

    // Convert to BCD format for RX8130
    uint8_t hour = ((timeinfo.tm_hour / 10) << 4) | (timeinfo.tm_hour % 10);
    uint8_t min = ((timeinfo.tm_min / 10) << 4) | (timeinfo.tm_min % 10);
    uint8_t sec = ((timeinfo.tm_sec / 10) << 4) | (timeinfo.tm_sec % 10);
    uint8_t year = (((timeinfo.tm_year % 100) / 10) << 4) | ((timeinfo.tm_year % 100) % 10);
    uint8_t month = (((timeinfo.tm_mon + 1) / 10) << 4) | ((timeinfo.tm_mon + 1) % 10);
    uint8_t day = ((timeinfo.tm_mday / 10) << 4) | (timeinfo.tm_mday % 10);
    uint8_t weekday = timeinfo.tm_wday;
    
    esp_err_t ret;
    
    // Update RTC time
    ret = rtcm_set_time(hour, min, sec);
    if (ret != ESP_OK) return ret;
    
    // Update RTC date
    ret = rtcm_set_date(year, month, day, weekday);
    return ret;
}

esp_err_t rtcm_sync_internet_time(void)
{
    // TZ is fixed at boot (sync_sys_time_apply_tz, issue #32) and the boot
    // sync_sys_time task owns the esp_netif_sntp singleton (SNTP sync + hourly RTC
    // refresh). Running a second init/wait/deinit here would tear that instance
    // down and kill periodic re-sync until reboot, so just push the already-synced
    // system time into the RTC. (The worldtimeapi.org offset lookup is gone too:
    // it froze TZ to a DST-less offset and failed whenever that API was down.)
    if (!dev_status_is_time_synced())
    {
        ESP_LOGE(TAG, "System time not SNTP-synced yet; RTC not updated");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = update_rtc_from_system_time();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to update RTC with synchronized time");
    }
    return ret;
}

esp_err_t rtcm_sync_system_time_from_rtc(void)
{
    uint8_t hour, min, sec;
    uint8_t year, month, day, weekday;
    esp_err_t ret;
    
    // Read current time and date from RTC
    ret = rtcm_get_time(&hour, &min, &sec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get time from RTC");
        return ret;
    }
    
    ret = rtcm_get_date(&year, &month, &day, &weekday);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get date from RTC");
        return ret;
    }
    
    // Convert BCD format to decimal
    uint8_t hour_dec = ((hour >> 4) & 0x0F) * 10 + (hour & 0x0F);
    uint8_t min_dec = ((min >> 4) & 0x0F) * 10 + (min & 0x0F);
    uint8_t sec_dec = ((sec >> 4) & 0x0F) * 10 + (sec & 0x0F);
    uint8_t year_dec = ((year >> 4) & 0x0F) * 10 + (year & 0x0F);
    uint8_t month_dec = ((month >> 4) & 0x0F) * 10 + (month & 0x0F);
    uint8_t day_dec = ((day >> 4) & 0x0F) * 10 + (day & 0x0F);

    // The RTC holds UTC -- convert with the UTC-aware path, never mktime (issue #32)
    time_t timestamp = rtcm_bcd_to_unix_timestamp(hour, min, sec, year, month, day);
    if (timestamp == 0) {
        ESP_LOGE(TAG, "Failed to convert RTC time to timestamp");
        return ESP_FAIL;
    }

    // Set system time
    struct timeval tv;
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;
    ret = settimeofday(&tv, NULL);
    
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to set system time from RTC: %d", ret);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "System time synchronized from RTC: %04d-%02d-%02d %02d:%02d:%02d", 
             2000 + year_dec, month_dec, day_dec, hour_dec, min_dec, sec_dec);
             
    return ESP_OK;
}

esp_err_t rtcm_init(i2c_port_t i2c_num)
{
    esp_err_t ret;

    rtcm_i2c = i2c_num;
    // Initialize RX8130 registers
    ret = rx8130_register_write(RX8130_REG_CTRL1, 0x00);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_CTRL2, 0xC7);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_EVT_CTRL, 0x04);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_EVT1, 0x00);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_EVT2, 0x40);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_EVT3, 0x10);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "RTC module initialized");
    return ESP_OK;
}
