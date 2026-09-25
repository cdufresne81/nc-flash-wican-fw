/*
 * This file is part of the WiCAN project.
 *
 * Copyright (C) 2022  Meatpi Electronics.
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

/* See flash_fence_logic.h. No ESP-IDF includes: tools/hosttest builds this with cc. */

#include "flash_fence_logic.h"

#include <stddef.h>

flash_fence_t flash_fence_eval(const flash_fence_in_t *in)
{
    if (in == NULL) { return FLASH_FENCE_CLEAR; }
    if (in->flash_active) { return FLASH_FENCE_FLASHING; }
    if (in->claim_raised && (!in->claim_expired || in->claim_owner_alive))
    {
        return FLASH_FENCE_SESSION;
    }
    return FLASH_FENCE_CLEAR;
}

flash_fence_t flash_fence_for_sd(flash_fence_t level)
{
    return (level == FLASH_FENCE_FLASHING) ? FLASH_FENCE_FLASHING : FLASH_FENCE_CLEAR;
}

bool flash_fence_reboot_may_fire(flash_fence_t level)
{
    return level != FLASH_FENCE_FLASHING;
}

bool flash_fence_write_may_start(bool reboot_pending, bool ota_active)
{
    return !reboot_pending && !ota_active;
}

const char *flash_fence_name(flash_fence_t level)
{
    switch (level)
    {
    case FLASH_FENCE_FLASHING: return "flashing";
    case FLASH_FENCE_SESSION:  return "session";
    default:                   return "clear";
    }
}

const char *flash_fence_message(flash_fence_t level)
{
    switch (level)
    {
    case FLASH_FENCE_FLASHING:
        return "An ECU flash is running. Wait for it to finish: interrupting it can leave the "
               "car's engine computer unable to start.";
    case FLASH_FENCE_SESSION:
        return "NC Flash is connected and using the car's ECU. Wait for it to finish, or close "
               "NC Flash and wait about a minute, then try again.";
    default:
        return "";
    }
}
