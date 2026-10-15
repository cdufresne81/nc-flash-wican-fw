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

#include "auto_pid_seed_logic.h"

#include <stddef.h>

auto_pid_seed_t auto_pid_seed_decide(const auto_pid_seed_in_t *in)
{
    if (in == NULL)
        return AUTO_PID_SEED_KEEP;

    /* A filesystem error is not "missing": never risk replacing a stored list. */
    if (in->stat_failed)
        return AUTO_PID_SEED_KEEP;
    if (!in->exists)
        return AUTO_PID_SEED_WRITE;
    /* No sensor file is this large; whatever it is, we did not write it and won't guess. */
    if (in->too_big)
        return AUTO_PID_SEED_KEEP;
    /* Empty or broken (e.g. a power cut during a Store): the Logger page can't load it. */
    if (!in->parsed)
        return AUTO_PID_SEED_REPLACE_BAD;
    /* Saved by this firmware: respect it, even an empty list. */
    if (in->has_format || in->has_calculated)
        return AUTO_PID_SEED_KEEP;
    /* Official firmware's file. Keep it if the user set up sensors there; our poller
     * reads that format. Replace it only when it holds nothing. */
    if (in->sensors > 0)
        return AUTO_PID_SEED_KEEP;
    return AUTO_PID_SEED_REPLACE_OFFICIAL;
}

const char *auto_pid_seed_name(auto_pid_seed_t action)
{
    switch (action)
    {
    case AUTO_PID_SEED_KEEP:             return "keep";
    case AUTO_PID_SEED_WRITE:            return "write";
    case AUTO_PID_SEED_REPLACE_BAD:      return "replace_bad";
    case AUTO_PID_SEED_REPLACE_OFFICIAL: return "replace_official";
    }
    return "?";
}
