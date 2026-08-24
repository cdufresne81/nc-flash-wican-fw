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

#ifndef POLL_GATE_LOGIC_H
#define POLL_GATE_LOGIC_H

/* Pure decision logic for poll_log's RECORDING gate -- the "is the engine actually turning"
 * answer that the CSV writer trusts through poll_log_gate_open().
 *
 * WHY THIS FILE EXISTS: the gate used to be an inline state machine inside polllog_eval_gate(),
 * behind ESP-IDF includes and file-scope volatiles. That made its two real bugs untestable
 * anywhere but on a car, and both of them shipped:
 *
 *   - an open CSV session force-set the gate to true, so the gate answered "the engine is
 *     running" with the meaning "we are already recording" -- a circle that kept one junk
 *     session alive for twelve hours on a bench with the engine stopped;
 *   - a stale RPM sample counted as "no RPM channel", so every pause longer than the freshness
 *     window bought a voltage-only opening on the very next pass.
 *
 * The functions here take their whole world as arguments and return a value. poll_log.c still
 * owns the task, the volatiles and every CAN byte; it gathers inputs, calls poll_gate_step()
 * and acts on the answer.
 *
 * ZERO ESP-IDF DEPENDENCIES, deliberately -- same rule and same reason as
 * components/csv_logger/csv_bringup_logic.h. tools/hosttest/run.sh compiles this with a stock
 * compiler and runs it against tools/hosttest/poll_gate_test.c. Adding an IDF include here
 * breaks that build, which is the point.
 */

#include <stdbool.h>
#include <stdint.h>

/* How many consecutive evaluation passes may be spent waiting for a FRESH rpm sample before the
 * gate gives up and opens on voltage alone.
 *
 * This is the bound on the one piece of added strictness in the whole design. A channel that has
 * genuinely died must still be able to record a drive -- losing a real drive is far worse than
 * logging a bench idle -- so the wait is short and always ends. Every sweep polls RPM (probe and
 * watch bypass the divisors outright, and an RPM row is never divisor-gated), so in practice the
 * fresh verdict lands on the very next pass and the wait costs one pass: ~40 ms in fast, <=1 s in
 * watch. */
#define POLLLOG_RPM_CONFIRM_SWEEPS 3u

/* Everything the gate carries between passes. Owned by the caller (one poll task, no locking). */
typedef struct
{
    bool    gate_open;      /* the engine answer, and the ONLY thing poll_log_gate_open() serves */
    int64_t low_since_us;   /* when the current not-running debounce started; 0 = not started */
    uint8_t stale_blocks;   /* consecutive passes blocked waiting for a fresh rpm sample */
} poll_gate_state_t;

/* One pass worth of inputs. Thresholds are passed in rather than defined here so poll_log.h keeps
 * the single source of truth for them (they are shared with the CSV writer and vehicle.h). */
typedef struct
{
    int64_t  now_us;
    bool     session_active;    /* a CSV session is open -- affects the sweep RATE, never the gate */
    bool     ecu_answering;     /* the ECU replied to one of OUR requests this session */
    bool     have_volts;        /* false = ADC unreadable; the gate then fails OPEN on voltage */
    float    volts;
    float    gate_volt_on;      /* engine_volt: the open threshold */
    float    hyst_v;            /* close threshold = gate_volt_on - hyst_v */
    uint32_t off_debounce_ms;   /* how long "not running" must hold before the gate closes */
    bool     rpm_configured;    /* the CONFIG carries an RPM channel that can deliver a sample */
    bool     rpm_known;         /* ...and the last sample is FRESH (caller applies the staleness) */
    bool     rpm_running;       /* ...and it is above the running threshold */
} poll_gate_in_t;

typedef enum
{
    POLL_GATE_NO_CHANGE = 0,
    POLL_GATE_OPENED,
    POLL_GATE_CLOSED,
    POLL_GATE_WAIT_RPM,   /* would have opened, but is waiting for a fresh rpm sample first */
} poll_gate_event_t;

typedef struct
{
    poll_gate_event_t event;
    bool gate_open;       /* the ENGINE answer */
    bool fast_sweep;      /* the RATE answer: gate_open || session_active */
    bool closed_on_rpm;   /* only meaningful on POLL_GATE_CLOSED: rpm stopped vs volts under band */
} poll_gate_out_t;

/* Advance the gate by one evaluation pass.
 *
 * THE TWO RULES THAT MUST NOT BE WEAKENED:
 *
 *  1. session_active never reaches gate_open. It only ORs into fast_sweep. An open session is a
 *     CONSEQUENCE of the gate, so letting it feed back in makes the gate self-sustaining: it can
 *     then only be closed by something outside the loop, which on a bench is nothing at all.
 *
 *  2. A missing rpm ANSWER is not the same as a missing rpm CHANNEL. rpm_configured && !rpm_known
 *     means "a channel exists and we have no fresh answer from it" -- the gate waits (bounded,
 *     see POLLLOG_RPM_CONFIRM_SWEEPS) rather than falling back to voltage. That covers both a
 *     channel that went quiet during a pause AND the first seconds of every boot, before any
 *     sample has landed. !rpm_configured means "there is no channel to wait for" and keeps the
 *     shipped voltage-only behaviour with no added delay.
 *
 *     This input is a CONFIG fact on purpose. An earlier version asked "has a sample ever
 *     arrived", which is false for the first seconds of every boot -- so the gate opened on
 *     voltage and wrote a junk session on every single boot, on a bench and equally on a car
 *     rebooted soon after a drive, where the battery still reads 13.2-13.5 V.
 *
 * While the gate is OPEN, staleness HOLDS the close debounce: it neither restarts it nor lets it
 * expire. Restarting on staleness is what would let an rpm channel that flip-flops fresh/stale
 * keep a gate open forever against a stopped engine; expiring on staleness is what would end a
 * real trip every time a host paused the poller. Only a positive running verdict restarts it, and
 * only a definite not-running verdict advances it. A configuration with no rpm channel at all
 * counts as positive so that voltage alone still holds the gate open, exactly as it ships. */
void poll_gate_step(poll_gate_state_t *st, const poll_gate_in_t *in, poll_gate_out_t *out);

/* Slam the gate shut and forget every accumulated timer.
 *
 * For the ONE caller that knows something the gate cannot: the quiesce path, which has just
 * proven the ECU is off the bus. An engine cannot run with its PCM silent, so there is nothing to
 * debounce. This exists so that path cannot clear poll_log's published s_gate_open while leaving
 * this state struct believing it is still open -- a divergence that would suppress the next real
 * CLOSED transition entirely. */
void poll_gate_force_close(poll_gate_state_t *st);

#endif /* POLL_GATE_LOGIC_H */
