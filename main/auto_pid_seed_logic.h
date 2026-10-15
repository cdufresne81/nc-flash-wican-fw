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

#ifndef AUTO_PID_SEED_LOGIC_H
#define AUTO_PID_SEED_LOGIC_H

/* Pure decision logic: at boot, should the default NC sensor list
 * (main/default_auto_pid.json) be written over /littlefs/auto_pid.json?
 *
 * WHY IT EXISTS: a WiCAN coming from the official MeatPi firmware already has an
 * auto_pid.json with "pids": [] -- the official Settings page saves the PID page on
 * every "Submit Changes", which a new owner presses to set up Wi-Fi. A normal web OTA
 * keeps the filesystem, so "seed only when the file is missing" left those users with
 * an empty Logger page.
 *
 * WHOSE FILE IS IT: our Logger page always writes "calculated" (since Task #17) and
 * "format" (AUTO_PID_FORMAT); the official firmware writes neither. A file with either
 * key was saved by this firmware and is never replaced, even when its list is empty.
 *
 * Free of ESP-IDF includes so tools/hosttest can run it on a PC. */

#include <stdbool.h>

typedef struct
{
    bool exists;          /* stat() found the file */
    bool stat_failed;     /* stat() failed with something other than ENOENT */
    bool too_big;         /* larger than we are willing to read at boot */
    bool parsed;          /* non-empty, valid JSON, and a JSON object */
    bool has_format;      /* top-level "format" key (written by our Logger page) */
    bool has_calculated;  /* top-level "calculated" key (written by our Logger page) */
    int  sensors;         /* pids + std_pids + every can_filters[].parameters entry */
} auto_pid_seed_in_t;

typedef enum
{
    AUTO_PID_SEED_KEEP = 0,         /* leave the file alone */
    AUTO_PID_SEED_WRITE,            /* no file: write the default */
    AUTO_PID_SEED_REPLACE_BAD,      /* empty or unreadable: back it up, write the default */
    AUTO_PID_SEED_REPLACE_OFFICIAL, /* official firmware's file with no sensors: back it up, write the default */
} auto_pid_seed_t;

auto_pid_seed_t auto_pid_seed_decide(const auto_pid_seed_in_t *in);

/* Stable name for logs. */
const char *auto_pid_seed_name(auto_pid_seed_t action);

#endif
