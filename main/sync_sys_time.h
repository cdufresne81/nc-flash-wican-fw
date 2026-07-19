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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Firmware-wide local timezone (POSIX TZ with DST rule), issue #32.
 * The system epoch and the RX8130 RTC always hold UTC -- TZ only affects how
 * localtime_r() renders it (trip filenames, CSV datetime column, event log).
 * Hardcoded for the single-owner NC fork; promote to a config key if the
 * device ever needs to travel time zones.
 */
#define SYNC_SYS_TIME_LOCAL_TZ "EST5EDT,M3.2.0,M11.1.0"

/**
 * @brief Apply SYNC_SYS_TIME_LOCAL_TZ to the C library (setenv TZ + tzset)
 *
 * Must run before anything renders wall-clock time: the RTC restore
 * (rtcm_sync_system_time_from_rtc), event_log_init and the CSV logger.
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