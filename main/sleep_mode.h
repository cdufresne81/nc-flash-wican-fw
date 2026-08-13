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


#ifndef SLEEP_MODE_h
#define SLEEP_MODE_h

#if HARDWARE_VER != WICAN_PRO
int8_t sleep_mode_init(uint8_t enable, float sleep_volt);
int8_t sleep_mode_get_voltage(float *val);
#elif HARDWARE_VER == WICAN_PRO
// #define HV_PRO_V140     1

// State machine states
typedef enum {
    STATE_NORMAL,
    STATE_LOW_VOLTAGE,
    STATE_SLEEPING,
    STATE_WAKE_PENDING
} system_state_t;

typedef struct {
    system_state_t state;
    float voltage;
    /* Milliseconds left before the device sleeps, and 0 in every state except STATE_LOW_VOLTAGE
     * (#85/#98). Read it as "remaining", never as "elapsed". Published together with `state` in one
     * struct copy so the two always agree; see the publish site in sleep_mode.c for why a shared
     * 64-bit deadline would tear instead. Sampled on the ~500 ms loop, so a reader can be up to one
     * pass stale -- fine for a countdown measured in minutes.
     *
     * A countdown can JUMP BACK UP: leaving STATE_LOW_VOLTAGE abandons it, and re-entering re-arms
     * the FULL sleep_time. Consumers must not assume this only decreases. */
    uint32_t timer;
    /* The configured countdown length in ms, as the sleep task ACTUALLY armed it -- published here
     * rather than re-read from config by each consumer. GET /sleep_status used to call
     * config_server_get_sleep_time() itself, which falls back to 0 on a bad read while this task
     * falls back to 120000 (2 min): the two then disagreed about the same number, and a reader
     * computing "how long has this run" as (total - remaining) got 0 and never noticed a countdown
     * at all. One publisher, one value. */
    uint32_t total_ms;
} sleep_state_info_t;

void sleep_mode_init(void);
esp_err_t sleep_mode_get_voltage(float *val);
esp_err_t sleep_mode_get_state(sleep_state_info_t *state_info);
void sleep_mode_print_wakeup_reason(void);

#endif


#endif
