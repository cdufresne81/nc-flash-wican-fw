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

#ifndef CSV_BRINGUP_LOGIC_H
#define CSV_BRINGUP_LOGIC_H

/* Pure decision logic for the CSV datalogger's bring-up and logging gate.
 *
 * WHY THIS FILE EXISTS: every decision below used to be an inline expression inside
 * csv_logger.c, behind FreeRTOS/ESP-IDF includes and file-scope statics, which made the
 * one class of bug that actually reached a customer -- "the datalogger silently does not
 * start" -- untestable anywhere but on a car. These functions take their whole world as
 * arguments and return a value. csv_logger.c still owns the RTC words, the tasks, the
 * queue and every byte of SD I/O; it passes pointers in here and acts on the answer.
 *
 * ZERO ESP-IDF DEPENDENCIES, deliberately: tools/hosttest/run.sh compiles this file with
 * a stock host compiler and runs it against the scenarios in
 * tools/hosttest/csv_bringup_test.c. Adding an IDF include here breaks that build, which
 * is the point -- the tests are the reason the file is separable at all.
 */

#include <stdbool.h>
#include <stdint.h>

/* ---- Manual override modes (web Start/Stop) --------------------------------
 * Shared with csv_logger.c rather than duplicated: the writer task compares against these
 * on every pass and the host tests assert on them, so a divergence would be silent. */
#define CSV_MANUAL_AUTO  0   /* follow the ignition gate (boot default) */
#define CSV_MANUAL_ON    1   /* force logging on  */
#define CSV_MANUAL_OFF   2   /* force logging off */

/* ---- RTC crash guard -------------------------------------------------------
 * Armed immediately before a bring-up attempt, cleared only once the writer has proven
 * stable. A boot that finds it still armed knows the previous attempt did not survive.
 * The word itself is RTC_NOINIT and lives in csv_logger.c; only its value lives here. */
#define CSV_ATTEMPT_MAGIC 0xA11C0DE5u

/* Consecutive armed-guard boots after which bring-up stops retrying for the uptime.
 * This is the boot-loop bound -- see csv_bringup_decide(). */
#define CSV_BRINGUP_MAX_SKIPS 2u

/* Writer uptime after which the crash guard is considered proven and cleared. */
#define CSV_GUARD_STABLE_US 15000000

typedef enum
{
    CSV_BRINGUP_START,       /* no crash pending: arm the guard and bring CSV up now      */
    CSV_BRINGUP_SKIP_RETRY,  /* previous attempt died: skip now, one delayed retry is due */
    CSV_BRINGUP_SKIP_FINAL,  /* it died again: no auto-start this uptime, no more retries */
} csv_bringup_decision_t;

/* The boot-time bring-up decision. Mutates the two RTC words and the skipped flag in
 * place -- every transition of this state machine happens in here.
 *
 * BOOT-LOOP BOUND (do not weaken): a deterministic CSV-init crash produces
 * START -> (crash) -> SKIP_RETRY -> (retry crashes) -> SKIP_FINAL, and stops. The device
 * is fully usable throughout; the worst case is one extra crash at >=60 s of uptime per
 * crash pair. Removing the bound restores the original boot loop this guard exists for.
 *
 * A boot that starts normally resets the skip chain: reaching START means the previous
 * boot either proved stable or never armed the guard at all, so the chain is not
 * "consecutive" any more. That also launders the garbage both RTC words hold on a cold
 * power-up, since a cold boot cannot find the magic and therefore always takes START.
 * The clamp is belt-and-braces for the astronomically unlikely cold boot whose garbage
 * guard word happens to BE the magic. */
csv_bringup_decision_t csv_bringup_decide(uint32_t *guard, uint32_t *skip_count, bool *skipped);

/* Re-arm the guard immediately before the delayed retry attempt. Deliberately does NOT
 * touch the skip count: the count is what bounds the retries, so the retry must not be
 * able to reset its own bound. */
void csv_bringup_arm_retry(uint32_t *guard);

/* Clear the whole chain after the writer has proven stable. The single place any of these
 * is allowed to be cleared outside csv_bringup_decide().
 *
 * `skipped` is the third word of this state machine, not an afterthought: it is what
 * sleep_mode_recovery_needed() reads to route a wake to the reboot repair channel instead
 * of resuming in place. It moves with the other two so no edit can clear the RTC pair and
 * leave a device resuming in place with a dead datalogger -- and so the host tests can
 * assert on it. */
void csv_bringup_mark_stable(uint32_t *guard, uint32_t *skip_count, bool *skipped);

/* Has the writer run long enough for the crash guard to be considered proven? */
bool csv_guard_clear_due(int64_t now_us, int64_t task_start_us);

/* The manual override mode for the next writer pass.
 *
 * A manual Stop means "stop this trip", not "disable auto-logging until someone reboots":
 * it clears back to AUTO once the ignition is off, so the next key-on records normally.
 * FORCE_ON is deliberately untouched -- bench work relies on it surviving a voltage that
 * flaps across the ignition threshold.
 *
 * A LEVEL rule, not an edge one, and that distinction is load-bearing: an edge-triggered
 * clear is consumed by the single ignition-off transition it sees, so if that transition
 * lands while a host holds the park lease the override never clears and Stop latches until
 * reboot again -- the original bug, narrowed rather than fixed. As a level, the clear
 * simply happens on the next pass after the exclusion lifts.
 *
 * datalog_parked is a HARD exclusion: while a host holds the datalog park lease the
 * forced-off state belongs to that host session and only datalog_restore_mode() may lift
 * it. An ignition cycle mid-flash must never restart the producer under a host. */
int8_t csv_manual_mode_next(int8_t mode, bool ignition_on, bool datalog_parked);

/* The logging gate, verbatim. Extracted only so the host tests can pin it: the v1.18
 * investigation cleared this expression, so any change to its truth table is a
 * regression, not a fix. */
bool csv_logging_active(bool sleep_requested, int8_t manual_mode,
                        bool ignition_on, bool engine_ok);

/* Milliseconds left on an auto-start countdown, clamped at zero.
 *
 * 32-bit and wraparound-safe ON PURPOSE. The deadline is written by the boot/retry path
 * and read by the httpd task on the other core, and a 64-bit read would TEAR on this
 * 32-bit core -- the same trap sleep_mode.c:1818-1824 documents having been caught by.
 * The signed difference means a deadline that straddles the uint32 ms rollover (~49 days
 * of uptime) still reports correctly rather than jumping to ~49 days remaining.
 *
 * There is no "0 means no countdown" sentinel: 0 is a legitimate uptime. Callers gate on
 * the auto-start state instead, which is the thing that actually knows. */
uint32_t csv_autostart_remaining_ms(uint32_t now_ms, uint32_t deadline_ms);

#endif /* CSV_BRINGUP_LOGIC_H */
