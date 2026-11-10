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

#ifndef DATALOG_SESSION_LOGIC_H
#define DATALOG_SESSION_LOGIC_H

/* Pure state and decisions for the NC Flash logging session: a temporary channel list the PC
 * hands the adapter for one logging run (docs/internals/datalog_session.md).
 *
 * WHY IT IS SEPARATE: everything that can strand a customer lives in these few rules -- a lease
 * that never expires leaves the adapter logging a list nobody saved, a token check that is too
 * loose lets a stale host end someone else's run, a snapshot taken twice loses the mode the
 * owner had chosen. They are decisions about numbers, so they are kept out of
 * datalog_session.c (which owns the lock, the HTTP handlers and every side effect) and pinned
 * by tools/hosttest/datalog_session_test.c.
 *
 * TIME is a 32-bit millisecond uptime, compared by signed difference. A lease is at most
 * DLS_TTL_MAX_S long, so the ~49-day rollover is harmless, and no reader on this 32-bit core can
 * see a torn 64-bit value (the trap csv_bringup_logic.h describes).
 *
 * ZERO ESP-IDF DEPENDENCIES, deliberately -- same rule as flash_fence_logic.c. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DLS_TTL_DEFAULT_S    30u            /* ttl_s when the host does not send one */
#define DLS_TTL_MIN_S        10u
#define DLS_TTL_MAX_S        120u
#define DLS_BODY_MAX_BYTES   (64 * 1024)    /* the full NC list is ~9 KB */
/* Deepest nesting a channel list may have. A real one is 5 deep (root > can_filters > filter >
 * parameters > parameter). The JSON parser recurses once per level, and this document is parsed
 * again on the poll task, so the depth is bounded before any parser sees it. */
#define DLS_JSON_MAX_DEPTH   16

/* Why the last session ended. Names (dls_end_name) are part of the HTTP contract. */
typedef enum {
    DLS_END_NONE = 0,   /* no session has ended since the current one began, or since boot */
    DLS_END_HOST,       /* "host":    op=end from the PC */
    DLS_END_EXPIRED,    /* "expired": the PC stopped sending keepalives */
    DLS_END_SLEEP,      /* "sleep":   the adapter went to sleep */
} dls_end_t;

typedef struct {
    bool      active;
    uint32_t  token;         /* never 0 while active; 0 is what a host with no token sends */
    uint32_t  ttl_ms;
    uint32_t  deadline_ms;   /* uptime ms at which the lease runs out */
    int8_t    saved_mode;    /* CSV manual mode to hand back when the session ends */
    dls_end_t last_end;
} dls_state_t;

/* ttl_s as the session will use it: the default when the host sent none, else clamped. */
uint32_t dls_clamp_ttl_s(bool given, uint32_t ttl_s);

/* May a session begin right now? BUSY and INACTIVE are both HTTP 409. */
typedef enum {
    DLS_BEGIN_OK = 0,
    DLS_BEGIN_BUSY,       /* something else owns the car's bus, or the adapter is going to sleep */
    DLS_BEGIN_INACTIVE,   /* the poller is not running, so nothing can swap the list in */
} dls_begin_check_t;

typedef struct {
    bool fence_clear;      /* config_server_flash_fence() == FLASH_FENCE_CLEAR */
    bool datalog_parked;   /* can_datalog_park_active() */
    bool bus_claimed;      /* can_host_bus_claim_active(): the RAW flag, see dls_begin_check */
    bool sleeping;         /* can_sleep_fence_active() */
    bool poll_active;      /* poll_log_active() */
} dls_begin_in_t;

/* BUSY wins over INACTIVE. The claim is tested raw as well as through the fence on purpose: a
 * claim a dead host left behind no longer fences a reboot, but it still parks the poller, and a
 * list that cannot be swapped in must not be accepted. */
dls_begin_check_t dls_begin_check(const dls_begin_in_t *in);

/* One plain sentence for the refusal ("" for DLS_BEGIN_OK). */
const char *dls_begin_refusal_msg(dls_begin_check_t why);

/* Begin, or replace, a session. Returns the new token (never 0, never the previous one).
 *
 * current_mode is saved ONLY when no session was active. A second begin is the same logging
 * run with a new list, and by then the "current" mode is the ON this session forced -- saving
 * it would make the session hand back its own override and leave the logger forced on for good.
 * seed is any number (a hardware random word on the device). */
uint32_t dls_begin(dls_state_t *st, uint32_t now_ms, uint32_t ttl_s, int8_t current_mode,
                   uint32_t seed);

/* Does this token name the active session? False for token 0 and when none is active. */
bool dls_token_matches(const dls_state_t *st, uint32_t token);

/* Renew the lease by its own TTL. False (nothing changed) on a stale token. A keepalive that
 * arrives after the deadline but before the reaper has acted still renews: the host is
 * evidently there. */
bool dls_keepalive(dls_state_t *st, uint32_t token, uint32_t now_ms);

/* Has the active session's lease run out? False when none is active. */
bool dls_expired(const dls_state_t *st, uint32_t now_ms);

/* End the active session and record why. Returns the manual mode to hand back. The caller has
 * already checked st->active (and the token, for a host end). */
int8_t dls_end(dls_state_t *st, dls_end_t reason);

/* Milliseconds left on the lease, 0 when none is active or it has run out. */
uint32_t dls_remaining_ms(const dls_state_t *st, uint32_t now_ms);

/* "host" / "expired" / "sleep", or "" for DLS_END_NONE. */
const char *dls_end_name(dls_end_t reason);

/* Is this JSON text nested no deeper than max_depth? Counts { and [ outside strings. It does not
 * validate the JSON; it only bounds what a recursive parser will be asked to do. */
bool dls_json_depth_ok(const char *json, size_t len, int max_depth);

#endif /* DATALOG_SESSION_LOGIC_H */
