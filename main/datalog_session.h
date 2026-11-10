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

#ifndef DATALOG_SESSION_H
#define DATALOG_SESSION_H

/* NC Flash logging session: a temporary channel list for one logging run.
 * What it is, the HTTP contract and the safety rules: docs/internals/datalog_session.md.
 *
 * In one paragraph: the PC posts the channels it wants to log. They live in RAM only and stand
 * in for auto_pid.json while the session lasts; the saved list is never written. The session
 * ends when the PC says so, when it stops sending keepalives, when the adapter sleeps, or at a
 * reboot -- and the adapter then goes back to its saved list by itself.
 *
 * WHAT IT CAN MAKE THE ADAPTER DO, and what enforces it. A session changes which requests the
 * poller sends and whether the CSV writer records -- nothing else. The requests are not taken on
 * trust: begin refuses any list with a row whose request would not start with service 01, 22
 * or 23 (the three read services; dls_pid_service_ok() in datalog_session_logic.c, judged on
 * the bytes sent, not on the row's Mode key), any list with std_pids, and any list outside
 * 1..DLS_PIDS_MAX_ROWS rows. The saved list written through /store_auto_data is NOT checked
 * that way and never was; that is a separate, older path.
 * It never touches FLASH_ACTIVE_BIT, a park or claim lease, SLCAN or a flash path. */

#include "esp_http_server.h"

/* Create the session lock (once) and register POST + GET /datalog_session on this server.
 * Called from config_server's URI registration, so the lock exists before any handler can
 * run; safe to call again when the server restarts. */
void datalog_session_register_handlers(httpd_handle_t server);

/* End the session if its lease has run out. Called once a second by datalog_lease_task; a
 * single flag read when no session is active. Never waits more than a few tens of ms: if a lock
 * it needs is busy it returns and the next tick tries again. */
void datalog_session_tick(void);

/* End the session because the adapter is going to sleep. A wake resumes in place with RAM
 * intact, so a session left active would come back with the temporary list still loaded and
 * the logger still forced on. Called from the sleep teardown, after the sleep fence is up (no
 * new session can begin behind it). No-op when no session is active. */
void datalog_session_end_for_sleep(void);

#endif /* DATALOG_SESSION_H */
