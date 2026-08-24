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

/* See poll_gate_logic.h for why this file has no ESP-IDF includes and must keep it that way. */

#include <stddef.h>

#include "poll_gate_logic.h"

void poll_gate_force_close(poll_gate_state_t *st)
{
    if (st == NULL)
        return;
    st->gate_open    = false;
    st->low_since_us = 0;
    st->stale_blocks = 0;
}

void poll_gate_step(poll_gate_state_t *st, const poll_gate_in_t *in, poll_gate_out_t *out)
{
    if (st == NULL || in == NULL || out == NULL)
        return;

    out->event         = POLL_GATE_NO_CHANGE;
    out->closed_on_rpm = false;

    /* One "is the engine running" answer, asked against a threshold that depends on which side of
     * the gate we are on: gate_volt_on to open, minus the band to close. That band IS the
     * hysteresis, so a voltage hovering on the line cannot flap the gate.
     *
     * An unreadable voltage does NOT hold the gate shut (have_volts false => volt_ok). Failing
     * open is the only choice that cannot turn a broken ADC into silently lost data. */
    const float volt_line = st->gate_open ? (in->gate_volt_on - in->hyst_v) : in->gate_volt_on;
    const bool  volt_ok   = !in->have_volts || (in->volts >= volt_line);

    /* rpm_ok answers "does rpm forbid running". A stale sample cannot forbid anything, which is
     * why the OPEN path needs the separate freshness wait below rather than just this term. */
    const bool rpm_ok = !in->rpm_known || in->rpm_running;

    /* No rpm channel CONFIGURED counts as positive: voltage alone must still be able to hold an
     * open gate open, which is what ships today and what C5 of the design protects. Declared up
     * here so no goto below jumps over its initialiser. */
    const bool rpm_positive = !in->rpm_configured || (in->rpm_known && in->rpm_running);

    if (!st->gate_open)
    {
        if (in->ecu_answering && volt_ok && rpm_ok)
        {
            /* RULE 2. We are about to open on voltage while a CONFIGURED rpm channel has no
             * fresh answer. That is the shape of every wrong opening seen on the bench, in two
             * flavours: a host pause starves the channel for longer than the freshness window and
             * the first pass after the resume opens before the next sweep can deliver the fresh
             * zero; and at boot, where no sample has landed yet at all. Wait for that sweep --
             * but only for a bounded number of passes, so a channel that really died still
             * records the drive. */
            if (in->rpm_configured && !in->rpm_known &&
                st->stale_blocks < POLLLOG_RPM_CONFIRM_SWEEPS)
            {
                st->stale_blocks++;
                out->event = POLL_GATE_WAIT_RPM;
                goto done;
            }
            st->gate_open    = true;
            st->low_since_us = 0;
            st->stale_blocks = 0;
            out->event       = POLL_GATE_OPENED;
        }
        else if (in->rpm_known)
        {
            /* A fresh sample arrived and it says stopped (or the volts/ECU terms refused). The
             * wait did its job; give the next stale window its full allowance again. */
            st->stale_blocks = 0;
        }
        goto done;
    }

    /* ---- gate is open ---------------------------------------------------------------------- */
    st->stale_blocks = 0;

    if (volt_ok && rpm_positive)
    {
        st->low_since_us = 0;      /* definitely running -> restart the debounce */
        goto done;
    }

    if (volt_ok && in->rpm_configured && !in->rpm_known)
    {
        /* Stale HOLD: the engine question has no answer this pass, so the debounce neither
         * restarts nor expires. A paused poller therefore cannot end a live trip, and a channel
         * flapping fresh/stale cannot keep a dead engine's gate open forever. */
        goto done;
    }

    /* A definite not-running verdict: volts under the band, or a fresh rpm below the threshold. */
    if (st->low_since_us == 0)
    {
        st->low_since_us = in->now_us;
        goto done;
    }
    if ((in->now_us - st->low_since_us) > (int64_t)in->off_debounce_ms * 1000)
    {
        st->gate_open      = false;
        st->low_since_us   = 0;
        out->closed_on_rpm = !rpm_ok;
        out->event         = POLL_GATE_CLOSED;
    }

done:
    out->gate_open  = st->gate_open;
    /* RULE 1, and the only place session_active is allowed to matter. */
    out->fast_sweep = st->gate_open || in->session_active;
}
