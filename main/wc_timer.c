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

#include <stdint.h>
#include <stdbool.h>
#include "wc_timer.h"
#include "esp_timer.h"

void wc_timer_set(wc_timer_t *timer, uint64_t expire_time_ms)
{
    *timer = esp_timer_get_time() + (expire_time_ms*1000);
}

bool wc_timer_is_expired(wc_timer_t *timer)
{
    return (*timer <= esp_timer_get_time()) ? true : false;
}

/* Milliseconds left before this timer expires, clamped at 0 once it has. Exists so callers that
 * need to REPORT a countdown (rather than just test it) do not have to know that a wc_timer_t is an
 * absolute microsecond deadline -- reaching inside the type spreads that assumption around and a
 * future change to the representation would then break silently instead of failing to compile. */
uint32_t wc_timer_remaining_ms(wc_timer_t *timer)
{
    const int64_t d_us = *timer - esp_timer_get_time();
    return (d_us > 0) ? (uint32_t)(d_us / 1000) : 0;
}
