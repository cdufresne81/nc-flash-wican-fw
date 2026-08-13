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

#ifndef SYNC_SYS_TIME_H
#define SYNC_SYS_TIME_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fallback local timezone (POSIX TZ with DST rule), issues #32 and #91.
 * The system epoch and the RX8130 RTC always hold UTC -- TZ only affects how
 * localtime_r() renders it (trip filenames, CSV datetime column, event log,
 * and the FAT mtimes stamped by the IDF's get_fattime()).
 * The live zone comes from the "timezone" key in config.json; this define is
 * only what we fall back to when that key is missing or invalid, and it keeps
 * the historical Eastern behaviour for a device that never sets one.
 */
#define SYNC_SYS_TIME_DEFAULT_TZ "EST5EDT,M3.2.0,M11.1.0"

/** Buffer size for a stored POSIX TZ string, including the NUL. */
#define SYNC_SYS_TIME_TZ_MAX 64

/**
 * @brief Sanity-check a POSIX TZ string before it reaches setenv()
 *
 * newlib never rejects a TZ string: garbage silently renders as UTC and a
 * half-parseable string renders at a plausible but wrong offset. So the check
 * has to happen before we apply it, not after. This is a character-class
 * sanity gate, not a full POSIX parser -- newlib remains the parser.
 *
 * @param tz candidate string (may be NULL)
 * @return true when the string is safe to apply
 */
bool sync_sys_time_tz_is_valid(const char *tz);

/**
 * @brief Apply the configured timezone to the C library (setenv TZ + tzset)
 *
 * Reads the "timezone" key straight out of config.json -- the config server
 * has not started this early -- and falls back to SYNC_SYS_TIME_DEFAULT_TZ if
 * the file, the key or the value is unusable. Strictly read-only: repairing a
 * broken config.json stays config_server_load_cfg()'s job.
 *
 * The caller must have mounted the internal filesystem (filesystem_init())
 * first. Must run before anything renders wall-clock time: the RTC restore
 * (rtcm_sync_system_time_from_rtc), event_log_init and the CSV logger -- and
 * before any task exists, since tzset() races a concurrent localtime_r().
 */
void sync_sys_time_apply_tz(void);

/**
 * @brief Initialize the system time synchronization task
 * 
 * This function creates a FreeRTOS task that handles SNTP time synchronization.
 * The task will:
 * - Wait for network connectivity
 * - Initialize SNTP with multiple servers for redundancy
 * - Perform initial time synchronization with retry mechanism
 * - Sync RTCM (Real-Time Clock Module) when time sync is successful
 * - Maintain periodic re-synchronization every hour
 * - Update RTCM during periodic sync to keep hardware clock accurate
 * 
 * The task is created with static allocation using PSRAM memory.
 */
void sync_sys_time_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SYNC_SYS_TIME_H */