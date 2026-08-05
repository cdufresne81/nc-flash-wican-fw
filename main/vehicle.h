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


#ifndef VEHICLE_H
#define VEHICLE_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ignition-voltage hysteresis band: the OFF edge sits this far under engine_on_volt. 3x the 0.1 V
 * ADC step. Shared, not copied: poll_log's recording gate applies the same band to decide when to
 * drop back to its slow watch sweep, and a device that disagrees with itself about when the engine
 * stopped would close one gate while the other stays open. */
#define VEHICLE_IGN_HYSTERESIS_V 0.3f

/* Engine-on voltage default, used when config_server_get_engine_volt() cannot supply one.
 * Shared for the same reason as the hysteresis band above: vehicle.c's ignition state machine,
 * poll_log's recording gate and main.c's vehicle_config all fall back to it independently, and a
 * device that disagrees with itself about the engine-on threshold would run one gate against
 * another. The string form is what config_server.c's parser stores, so the two must stay equal --
 * keep them on adjacent lines so a retune cannot move one without seeing the other.
 * NOTE: 13.0 is also the low end of the accepted range; the range check and the web slider's
 * min= both encode it, so lowering this default means moving those too. */
#define VEHICLE_ENGINE_ON_VOLT_DEFAULT      13.0f
#define VEHICLE_ENGINE_ON_VOLT_DEFAULT_STR  "13.0"

// Vehicle event bits
#define VEHICLE_IGNITION_ON_BIT     BIT0
#define VEHICLE_STATIONARY_BIT      BIT1

typedef enum {
    VEHICLE_STATE_IGNITION_OFF,
    VEHICLE_STATE_IGNITION_ON,
    VEHICLE_STATE_IGNITION_INVALID
} vehicle_ignition_state_t;

typedef enum {
    VEHICLE_MOTION_STATIONARY,
    VEHICLE_MOTION_ACTIVE,
    VEHICLE_MOTION_INVALID
} vehicle_motion_state_t;

typedef struct {
    float engine_on_volt;   /* engine-running detect threshold (V); OFF edge = on - VEHICLE_IGN_HYSTERESIS_V */

} vehicle_config_t;




// typedef struct {
//     bool ignition_on;
//     bool stationary;
//     float voltage;
// } vehicle_state_t;


void vehicle_init(vehicle_config_t *vehicle_config);
vehicle_ignition_state_t vehicle_ignition_state(void);
vehicle_motion_state_t vehicle_motion_state(void);

#endif 
