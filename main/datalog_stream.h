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

/*
 * Live datalog stream (issue #3): a read-only TCP tail of the trip CSV the writer puts on SD,
 * so a PC tool can follow the log while driving. Port 35002, always on, one client at a time
 * (a new connection replaces the old one, which is told "#bye replaced"). The device only
 * serves; the client sends nothing.
 *
 * Wire protocol NCDLv1. Every line ends in '\n'. Lines starting with '#' are control lines and
 * a receiver must ignore any it does not know. Every other line is CSV, byte-identical to the
 * SD file.
 *
 *   #hello NCDLv1 fw=<git_version>   first line on every connection
 *   #idle                            no trip is open right now
 *   #session file=<name> cols=<n>    a trip is open (sent on connect and when one opens or
 *                                    rotates); the NEXT line is its CSV header
 *   #nohdr                           sent instead of the header when it is unavailable
 *   #drop <n>                        n rows were skipped here because the client fell behind
 *   #close                           the trip closed; the socket stays open
 *   #ping                            nothing sent for 3 s; lets the client spot a dead link
 *   #bye replaced                    a newer client took over; do not reconnect
 *
 * Live only: rows written before the client connected are not replayed. The SD file stays the
 * complete record; the stream is a view of it.
 *
 * Nothing here touches the CAN bus, the coexistence leases or the flash paths, and a connected
 * client is not "host present" for the dead-man reaper.
 */

#ifndef __DATALOG_STREAM_H__
#define __DATALOG_STREAM_H__

#include <stdbool.h>
#include <stdint.h>

/* Fixed port; must match the host's WICAN_DATALOG_STREAM_PORT. */
#define WICAN_DATALOG_STREAM_PORT   35002

/* Allocate the stream state (PSRAM only) and register the CSV writer hook. Call BEFORE
 * csv_logger_start_at_boot() so the first trip's header is captured. Returns 0 on success; on
 * failure the logger runs exactly as it would without a stream. */
int datalog_stream_init(void);

/* Start the listener task. Call after the network stack is up. Returns 0 on success. */
int datalog_stream_start(uint16_t port);

bool datalog_stream_client_connected(void);
uint32_t datalog_stream_rows_sent(void);
uint32_t datalog_stream_rows_dropped(void);

#endif
