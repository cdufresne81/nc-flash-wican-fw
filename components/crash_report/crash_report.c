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
#include <stdio.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_private/panic_internal.h"

#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include "xtensa_context.h"     // XtExcFrame
#include "esp_cpu_utils.h"      // esp_cpu_process_stack_pc()
#include "esp_debug_helpers.h"  // esp_backtrace_frame_t, esp_backtrace_get_next_frame()
#endif

#include "event_log.h"
#include "crash_report.h"

// Distinct RTC_NOINIT magic (NOT csv 0xA11C0DE5 / poll_log 0x9011106D / fast_log 0xFA571A6D /
// event_log 0xE7106A11). Marks a fully-written crash snapshot; any other value = nothing to report.
#define CRASH_REPORT_MAGIC  0xC0FFEE10u
#define CRASH_BT_MAX        16      // captured call-stack depth (fits several event_log lines)

typedef struct {
    uint32_t magic;
    int32_t  core;       // core that took the exception
    int32_t  exception;  // panic_exception_t (portable, arch-independent)
    uint32_t exccause;   // arch exception cause code
    uint32_t pc;         // raw faulting PC (matches the panic dump's "PC :" register line)
    uint32_t depth;      // valid entries in bt[]
    uint32_t bt[CRASH_BT_MAX];  // processed call-stack PCs (match the "Backtrace:" line)
} crash_record_t;

// RTC_NOINIT: survives the panic's SW-reset reboot (but NOT a cold power cycle) -- the same mechanism
// the csv/poll/fast/event crash guards rely on. Lives in RTC RAM, which is NOT behind the flash cache,
// so the IRAM panic-context path below can write it even if the cache is disabled.
RTC_NOINIT_ATTR static crash_record_t s_crash;

/* ---- panic-context capture (linker-wrapped esp_panic_handler) --------------------------------- *
 * Runs INSIDE the fatal-error handler: interrupts off, scheduler dead, watchdog armed. Brick-safe
 * rules mirror csv_logger/event_log's reset-path discipline -- NO allocation, NO FreeRTOS calls, NO
 * printf, NO SD, NO flash writes. It only reads the exception frame and writes RTC RAM, then chains to
 * the real handler (which prints the dump and reboots). IRAM_ATTR so a cache-error panic (flash cache
 * disabled) can still run it; esp_backtrace_get_next_frame() is likewise IRAM-resident. */
void __real_esp_panic_handler(panic_info_t *info);

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    if (info != NULL) {
        s_crash.magic     = 0;                  // invalidate until the record is fully written
        s_crash.core      = info->core;
        s_crash.exception = (int32_t)info->exception;
        s_crash.exccause  = 0;
        s_crash.pc        = 0;
        uint32_t depth    = 0;

#if CONFIG_IDF_TARGET_ARCH_XTENSA
        if (info->frame != NULL) {
            const XtExcFrame *xt = (const XtExcFrame *)info->frame;
            s_crash.exccause = xt->exccause;
            s_crash.pc       = xt->pc;          // raw exception PC

            // Walk the crashing stack exactly as esp_backtrace_print_from_frame() does: seed from the
            // exception frame (pc, sp=a1, next_pc=a0) and record esp_cpu_process_stack_pc() per frame.
            esp_backtrace_frame_t stk = {
                .pc = xt->pc, .sp = xt->a1, .next_pc = xt->a0, .exc_frame = xt,
            };
            s_crash.bt[depth++] = esp_cpu_process_stack_pc(stk.pc);
            while (depth < CRASH_BT_MAX && stk.next_pc != 0) {
                if (!esp_backtrace_get_next_frame(&stk)) {
                    break;                      // hit a corrupt/last frame
                }
                s_crash.bt[depth++] = esp_cpu_process_stack_pc(stk.pc);
            }
        }
#endif
        s_crash.depth = depth;
        s_crash.magic = CRASH_REPORT_MAGIC;     // publish: record complete
    }

    __real_esp_panic_handler(info);             // normal dump + reboot (never returns)
}

/* ---- boot-time replay (normal task context) --------------------------------------------------- */

static const char *crash_exc_str(int32_t e)
{
    switch (e) {
        case PANIC_EXCEPTION_DEBUG: return "DEBUG";
        case PANIC_EXCEPTION_IWDT:  return "INT_WDT";
        case PANIC_EXCEPTION_TWDT:  return "TASK_WDT";
        case PANIC_EXCEPTION_ABORT: return "ABORT";
        case PANIC_EXCEPTION_FAULT: return "FAULT";
        default:                    return "UNKNOWN";
    }
}

void crash_report_emit_pending(void)
{
    if (s_crash.magic != CRASH_REPORT_MAGIC) {
        return;   // previous boot did not crash, or the crash was already reported
    }

    // Snapshot then clear immediately: report exactly once, never re-report after a clean reboot.
    crash_record_t c = s_crash;
    s_crash.magic = 0;

    event_log_emit(EVL_INFO, "CRASH %s core=%ld pc=0x%08lx cause=%lu frames=%lu",
                   crash_exc_str(c.exception), (long)c.core,
                   (unsigned long)c.pc, (unsigned long)c.exccause, (unsigned long)c.depth);

    // Backtrace PCs, chunked to fit one event_log detail line. Feed these to
    // xtensa-esp32s3-elf-addr2line against the matching build ELF to get file:line.
    const uint32_t per_line = 6;
    for (uint32_t i = 0; i < c.depth; i += per_line) {
        char buf[112];
        int n = snprintf(buf, sizeof(buf), "CRASH bt[%lu]:", (unsigned long)i);
        for (uint32_t j = i; j < c.depth && j < i + per_line; j++) {
            if (n < 0 || n >= (int)sizeof(buf)) {
                break;
            }
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " 0x%08lx", (unsigned long)c.bt[j]);
        }
        event_log_emit(EVL_INFO, "%s", buf);
    }
}
