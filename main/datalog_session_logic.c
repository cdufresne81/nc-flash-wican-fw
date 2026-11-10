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

/* See datalog_session_logic.h. No ESP-IDF includes: tools/hosttest builds this with cc. */

#include "datalog_session_logic.h"

uint32_t dls_clamp_ttl_s(bool given, uint32_t ttl_s)
{
    if (!given)                { return DLS_TTL_DEFAULT_S; }
    if (ttl_s < DLS_TTL_MIN_S) { return DLS_TTL_MIN_S; }
    if (ttl_s > DLS_TTL_MAX_S) { return DLS_TTL_MAX_S; }
    return ttl_s;
}

dls_begin_check_t dls_begin_check(const dls_begin_in_t *in)
{
    if (in == NULL) { return DLS_BEGIN_BUSY; }
    if (!in->fence_clear || in->datalog_parked || in->bus_claimed || in->sleeping)
    {
        return DLS_BEGIN_BUSY;
    }
    if (!in->poll_active) { return DLS_BEGIN_INACTIVE; }
    return DLS_BEGIN_OK;
}

const char *dls_begin_refusal_msg(dls_begin_check_t why)
{
    switch (why)
    {
    case DLS_BEGIN_BUSY:
        return "NC Flash is using the car's ECU, or the adapter is going to sleep. "
               "Logging can start when that is finished.";
    case DLS_BEGIN_INACTIVE:
        return "The datalogger is not running on this adapter, so it cannot switch channel lists.";
    default:
        return "";
    }
}

uint32_t dls_begin(dls_state_t *st, uint32_t now_ms, uint32_t ttl_s, int8_t current_mode,
                   uint32_t seed)
{
    uint32_t token = seed;
    /* 0 means "no token", and a replace must not hand the host the token it already has: it
     * could not tell the two lists apart. At most two steps, since only two values are barred. */
    while (token == 0u || token == st->token)
    {
        token++;
    }

    if (!st->active)
    {
        st->saved_mode = current_mode;
        st->last_end = DLS_END_NONE;
    }
    st->active = true;
    st->token = token;
    st->ttl_ms = ttl_s * 1000u;
    st->deadline_ms = now_ms + st->ttl_ms;
    return token;
}

bool dls_token_matches(const dls_state_t *st, uint32_t token)
{
    return st->active && token != 0u && token == st->token;
}

bool dls_keepalive(dls_state_t *st, uint32_t token, uint32_t now_ms)
{
    if (!dls_token_matches(st, token)) { return false; }
    st->deadline_ms = now_ms + st->ttl_ms;
    return true;
}

bool dls_expired(const dls_state_t *st, uint32_t now_ms)
{
    return st->active && ((int32_t)(st->deadline_ms - now_ms) <= 0);
}

int8_t dls_end(dls_state_t *st, dls_end_t reason)
{
    st->active = false;
    st->last_end = reason;
    /* The token is kept on purpose: dls_begin() then never reissues it, so a host still
     * holding it cannot be mistaken for the owner of the next session. */
    return st->saved_mode;
}

uint32_t dls_remaining_ms(const dls_state_t *st, uint32_t now_ms)
{
    if (!st->active) { return 0u; }
    const int32_t left = (int32_t)(st->deadline_ms - now_ms);
    return (left > 0) ? (uint32_t)left : 0u;
}

const char *dls_end_name(dls_end_t reason)
{
    switch (reason)
    {
    case DLS_END_HOST:    return "host";
    case DLS_END_EXPIRED: return "expired";
    case DLS_END_SLEEP:   return "sleep";
    default:              return "";
    }
}

bool dls_json_depth_ok(const char *json, size_t len, int max_depth)
{
    int  depth = 0;
    bool in_string = false;
    bool escaped = false;

    if (json == NULL) { return false; }
    for (size_t i = 0; i < len; i++)
    {
        const char c = json[i];
        if (in_string)
        {
            if (escaped)        { escaped = false; }
            else if (c == '\\') { escaped = true; }
            else if (c == '"')  { in_string = false; }
            continue;
        }
        if (c == '"')
        {
            in_string = true;
        }
        else if (c == '{' || c == '[')
        {
            if (++depth > max_depth) { return false; }
        }
        else if (c == '}' || c == ']')
        {
            if (depth > 0) { depth--; }
        }
    }
    return true;
}
