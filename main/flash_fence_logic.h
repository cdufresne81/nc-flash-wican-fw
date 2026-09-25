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

#ifndef FLASH_FENCE_LOGIC_H
#define FLASH_FENCE_LOGIC_H

/* Pure decision logic for the ECU-flash fence (#145): may a person or a tool reboot, update or
 * reconfigure the adapter, or touch the SD card, right now?
 *
 * WHY IT EXISTS: the adapter always knew when a flash was running (FLASH_ACTIVE_BIT), but only the
 * sleep path ever asked. A web Reboot, a config save, a firmware upload, a console `reboot` or a
 * file-manager delete of the staged ROM would all go ahead in the middle of TransferData, and
 * interrupting that can leave the PCM unbootable.
 *
 * Two levels, because the two hazards are not the same size:
 *
 *   FLASHING  A flash/read codec owns the bus. Interrupting it can brick the ECU. Everything is
 *             refused, with no override, and the reboot timer itself waits (see
 *             flash_fence_reboot_may_fire).
 *   SESSION   NC Flash holds a LIVE bus-claim lease (authentication, DTCs, RAM work, or the
 *             seconds before it starts a write). Interrupting it cannot brick anything, but it
 *             kills the session and can race the write that is about to start, so reboots,
 *             updates and config saves are refused. SD-card operations are NOT: NC Flash itself
 *             uploads the staged ROM over /upload/sd while it holds the claim.
 *
 * "Live" is the lease test the sleep backstop uses (sleep_mode_teardown), never the raw claim flag:
 * the raw flag stays up until the dead-man reaper clears it, and the reaper waits for the bus to go
 * quiet, which key-on never happens. Fencing on the raw flag would lock the owner out of Reboot
 * for as long as the key is on after NC Flash crashed. The lease drops within its 75 s TTL once
 * the owning 35001 socket is gone. A host that FREEZES with its socket still open holds it for as
 * long as it stays frozen; closing NC Flash, or POST /datalog?op=bus_release, clears it.
 *
 * ZERO ESP-IDF DEPENDENCIES, deliberately -- same rule as csv_bringup_logic.c and
 * poll_gate_logic.c, so tools/hosttest can build it with a stock compiler. */

#include <stdbool.h>

typedef enum {
    FLASH_FENCE_CLEAR = 0,
    FLASH_FENCE_SESSION,    /* live NC Flash bus-claim, no codec on the bus */
    FLASH_FENCE_FLASHING,   /* a flash/read codec owns the bus -- the brick case */
} flash_fence_t;

typedef struct {
    bool flash_active;        /* can_flash_active(): FLASH_ACTIVE_BIT */
    bool claim_raised;        /* the raw host bus-claim flag */
    bool claim_expired;       /* the claim lease's deadline has passed */
    bool claim_owner_alive;   /* the 35001 connection that armed the claim is still open */
} flash_fence_in_t;

/* The current fence level. FLASHING wins over SESSION. */
flash_fence_t flash_fence_eval(const flash_fence_in_t *in);

/* Level for actions that only threaten a RUNNING flash: SD-card mutations. A session is let
 * through (NC Flash stages its ROM over /upload/sd while it holds the claim). */
flash_fence_t flash_fence_for_sd(flash_fence_t level);

/* The scheduled-reboot timer's backstop. A reboot that was accepted a moment before a flash began
 * must wait for the flash to end, not fire into it. Only FLASHING holds it: a session cannot
 * brick, and holding a reboot on a frozen host's session would hold it forever. */
bool flash_fence_reboot_may_fire(flash_fence_t level);

/* The fast-write codec's start check. The reverse of the HTTP fence: a flash must not start while
 * the adapter is about to reboot or is mid-way through a firmware update (the update has already
 * disabled CAN, and finishes with a reboot). */
bool flash_fence_write_may_start(bool reboot_pending, bool ota_active);

/* Stable machine name for JSON: "clear", "session", "flashing". */
const char *flash_fence_name(flash_fence_t level);

/* One plain-English sentence for a person, for the level that refused them. */
const char *flash_fence_message(flash_fence_t level);

#endif /* FLASH_FENCE_LOGIC_H */
