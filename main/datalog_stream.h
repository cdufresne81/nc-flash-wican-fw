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

#ifndef __DATALOG_STREAM_H__
#define __DATALOG_STREAM_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "csv_logger.h"   /* csv_stream_event_t / csv_stream_hook_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Always-on live-datalog TCP stream listener (issue #3, "NCDLv1").
 *
 * A tail-only server that mirrors the wide-CSV datalog the device is writing to SD onto a
 * raw-TCP socket, so NC Flash (or MegaLogViewerHD via a local file tail) can watch the log
 * live over Wi-Fi. NC Flash always initiates; the device only ever serves. Deliberately a
 * 3-task clone of slcan_port.c (server accept loop, rx task for disconnect detection, tx
 * drain task) so it shares NO state with the flash/coexistence path -- it touches no CAN
 * bit, no park/claim/lease, and adds no config key.
 *
 * The data source is the csv writer task: main/ registers datalog_stream_csv_hook() into
 * csv_logger via csv_logger_set_stream_hook(); each header/row/close event is enqueued into
 * an internal-RAM StreamBuffer (single writer = csv task, single reader = tx task) with
 * 0-timeout, drop-newest-whole-line backpressure so the writer can NEVER block.
 *
 * Wire protocol (device -> host; host sends nothing, all lines '\n'-terminated):
 *   #hello NCDLv1 fw=<git_version>   once, immediately on accept.
 *   #idle                            accept with no CSV session active.
 *   #session file=<basename> cols=<n>  a session is/becomes active; followed by the header line.
 *   #nohdr                           header line unavailable (joiner with an incomplete copy,
 *                                    or a header line > 4 KB). Rows still follow.
 *   #drop <n>                        n whole lines were dropped (ring full) since the last good line.
 *   #close                           the SD session closed. Socket stays open.
 * Everything else is a raw CSV line byte-identical to the SD file.
 */

/* Fixed TCP port of the live-datalog stream listener. Single source of truth: main.c and
 * config_server.c both use this define. 35002 is unclaimed (35000 stock, 35001 dedicated
 * SLCAN); MUST match the host's src/ecu/constants.py. */
#define WICAN_DATALOG_STREAM_PORT   35002

/* Start the listener on `port` (3 PSRAM-stacked tasks at prio 5). WICAN_PRO only.
 * Returns 0 on success, -1 on task/alloc failure. */
int8_t datalog_stream_init(uint32_t port);

/* csv_logger stream hook: register into csv_logger via csv_logger_set_stream_hook() at boot.
 * Runs on the csv writer task; non-blocking, fast-returns when no client is connected. */
void datalog_stream_csv_hook(csv_stream_event_t ev, const char *data, size_t len);

/* Status getters for GET /check_status. */
bool     datalog_stream_client_connected(void);   /* a client socket is connected */
uint32_t datalog_stream_rows_sent(void);          /* data rows successfully enqueued */
uint32_t datalog_stream_rows_dropped(void);       /* data rows dropped (ring full) */

#ifdef __cplusplus
}
#endif

#endif /* __DATALOG_STREAM_H__ */
