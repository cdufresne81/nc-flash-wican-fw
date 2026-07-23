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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Serial-free post-hoc crash reporter.
//
// The OBD-PRO's USB-C is a USB *host* port at runtime (main.c: usb_host_init()), so a PC never sees a
// serial console and the standard panic backtrace is unrecoverable over the cable. This module captures
// it anyway, over WiFi: a linker-wrapped esp_panic_handler (see crash_report.c) snapshots the faulting
// backtrace into RTC_NOINIT RAM -- which survives the panic's SW-reset reboot -- and the boot-time
// replay below re-emits it into the event_log, retrievable via GET /event_log. Decode the captured PCs
// offline with xtensa-esp32s3-elf-addr2line against the matching build ELF.

// Replay a crash captured on the *previous* boot into the event_log (as EVL_INFO "CRASH ..." lines),
// then clear it so it is reported exactly once. Cheap no-op if the previous boot did not crash.
//
// Call once early in app_main, right AFTER event_log_init() and the EVL_BOOT line, so the crash lands
// adjacent to the boot it preceded and gets flushed to SD by the event_log writer.
void crash_report_emit_pending(void);

#ifdef __cplusplus
}
#endif
