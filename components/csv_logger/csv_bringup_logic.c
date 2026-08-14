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

/* See csv_bringup_logic.h for why these decisions live outside csv_logger.c.
 * Keep this file free of ESP-IDF includes -- tools/hosttest/run.sh builds it with a
 * stock host compiler. */

#include "csv_bringup_logic.h"

csv_bringup_decision_t csv_bringup_decide(uint32_t *guard, uint32_t *skip_count, bool *skipped)
{
    if (*guard == CSV_ATTEMPT_MAGIC)
    {
        /* The previous attempt armed this and never lived long enough to clear it.
         * Disarm first, unconditionally: whatever we decide below, the NEXT boot must
         * get a clean attempt rather than inheriting a guard nobody will ever clear.
         * That disarm is what stops a single crash costing CSV logging permanently. */
        *guard = 0;

        uint32_t n = *skip_count;
        if (n > CSV_BRINGUP_MAX_SKIPS)
        {
            n = CSV_BRINGUP_MAX_SKIPS;   /* RTC garbage; see the header's cold-boot note */
        }
        n++;
        *skip_count = n;

        *skipped = true;
        return (n < CSV_BRINGUP_MAX_SKIPS) ? CSV_BRINGUP_SKIP_RETRY : CSV_BRINGUP_SKIP_FINAL;
    }

    *guard = CSV_ATTEMPT_MAGIC;
    *skip_count = 0;
    return CSV_BRINGUP_START;
}

void csv_bringup_arm_retry(uint32_t *guard)
{
    *guard = CSV_ATTEMPT_MAGIC;
}

void csv_bringup_mark_stable(uint32_t *guard, uint32_t *skip_count, bool *skipped)
{
    *guard = 0;
    *skip_count = 0;
    *skipped = false;
}

bool csv_guard_clear_due(int64_t now_us, int64_t task_start_us)
{
    return (now_us - task_start_us) > CSV_GUARD_STABLE_US;
}

int8_t csv_manual_mode_next(int8_t mode, bool ignition_on, bool datalog_parked)
{
    if (mode == CSV_MANUAL_OFF && !ignition_on && !datalog_parked)
    {
        return CSV_MANUAL_AUTO;
    }
    return mode;
}

bool csv_logging_active(bool sleep_requested, int8_t manual_mode,
                        bool ignition_on, bool engine_ok)
{
    return sleep_requested                ? false
         : (manual_mode == CSV_MANUAL_ON)  ? true
         : (manual_mode == CSV_MANUAL_OFF) ? false
                                           : (ignition_on && engine_ok);
}

uint32_t csv_autostart_remaining_ms(uint32_t now_ms, uint32_t deadline_ms)
{
    const int32_t remaining = (int32_t)(deadline_ms - now_ms);
    return (remaining > 0) ? (uint32_t)remaining : 0u;
}
