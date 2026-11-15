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

#ifndef EXT_CLIENT_LOGIC_H
#define EXT_CLIENT_LOGIC_H

/* Pure decision logic for "an external client is using the car": an ELM327 app today (Torque,
 * Car Scanner, OBD Fusion on TCP 35000), a J2534 tester or a BLE app later.
 *
 * THE RULE, in one place:
 *
 *   1. While such a client is ACTIVE the standalone datalogger is benched. The client talks to
 *      the car through the MIC3624 interpreter chip; poll_log talks through the ESP32's own TWAI
 *      controller. They are two testers on one bus asking the same ECU (0x7E0), so each would
 *      read the other's answers. One of them has to stop, and it is the logger.
 *   2. ACTIVE means "sent us a byte within the hold window". Nothing arms, renews or releases:
 *      the hold expires on its own, so an app that crashes, a phone that walks away or a socket
 *      that never closes all end the same way -- the logger resumes ten seconds later. There is
 *      no lease to leak and no reaper to get wrong.
 *   3. NC Flash always wins. While a flash codec, a host bus-claim or a host datalog pause holds
 *      the bus, an external client's command is refused with the ELM327 "BUS BUSY" answer and
 *      never reaches the chip. Sleep teardown refuses it the same way.
 *
 * ZERO ESP-IDF DEPENDENCIES, deliberately -- same rule as flash_fence_logic.c, so tools/hosttest
 * can build it with a stock compiler. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How long after its last byte a client still counts as active. Matches the figure upstream
 * WiCAN uses for the same job (the poller yields for 10 s after the last app byte): long enough
 * to cover an app's slowest refresh, short enough that logging is back before anyone notices. */
#define EXT_CLIENT_HOLD_MS   (10000U)

/* The hold state: one flag and one millisecond stamp. `seen` exists so that a device that never
 * had a client is not "active" for the first ten seconds after boot (stamp 0, clock near 0), and
 * so that a stamp left behind does not come back to life when the 32-bit clock wraps. */
typedef struct {
    bool     seen;       /* a client has sent something since the hold last lapsed */
    uint32_t last_ms;    /* millisecond clock at its most recent byte */
} ext_client_hold_t;

/* A client byte arrived. */
void ext_client_touch(ext_client_hold_t *h, uint32_t now_ms);

/* Is the hold in force? Wrap-safe: the subtraction is unsigned, so it is right across the 49-day
 * rollover of a millisecond clock. Does not modify the state. */
bool ext_client_active(const ext_client_hold_t *h, uint32_t now_ms);

/* Milliseconds since the last client byte; UINT32_MAX when there has been none. */
uint32_t ext_client_idle_ms(const ext_client_hold_t *h, uint32_t now_ms);

/* Drop a lapsed hold's `seen` flag (rule 2's wrap guard). Call it from the slow path; it is a
 * no-op while the hold is in force. Returns true when it cleared the flag. */
bool ext_client_expire(ext_client_hold_t *h, uint32_t now_ms);

/* May an external client's command go to the interpreter chip right now? */
typedef enum {
    EXT_CLIENT_ADMIT = 0,
    EXT_CLIENT_REFUSE_FLASH,     /* a flash/read codec owns the bus -- the brick case */
    EXT_CLIENT_REFUSE_SESSION,   /* NC Flash holds a bus-claim or a datalog pause */
    EXT_CLIENT_REFUSE_SLEEP,     /* the device is tearing down for sleep */
} ext_client_verdict_t;

typedef struct {
    bool flash_active;   /* can_flash_active() */
    bool host_claim;     /* can_host_bus_claim_active() */
    bool host_park;      /* can_datalog_park_active() */
    bool sleep_fence;    /* can_sleep_fence_active() */
} ext_client_gate_in_t;

ext_client_verdict_t ext_client_gate(const ext_client_gate_in_t *in);

/* Stable name for logs and JSON. Never NULL. */
const char *ext_client_verdict_name(ext_client_verdict_t v);

/* How many complete commands (carriage returns) a received chunk carries. The router needs it to
 * know how many queue slots the chunk will take BEFORE it commits to queueing any of it. */
size_t ext_client_count_commands(const uint8_t *buf, size_t len);

/* Is this a command a client must not send to the interpreter chip? `cmd` is one complete line
 * as the client typed it (any case, any spacing, with or without its CR); `len` bounds it, and a
 * NUL inside it ends it early.
 *
 * Two families are refused, and nothing else:
 *
 *   - Commands that change what the chip remembers or how the firmware reaches it: programmable
 *     parameters (AT PP -- obd.c sets 0E/0F for sleep, and they persist in EEPROM), low power,
 *     the UART baud rate (the firmware would lose the chip), sleep configuration, factory reset.
 *   - Bus monitors (AT MA / MR / MT / MP, ST M...). They stream until interrupted and never
 *     print a prompt, so they would hold the chip, and the UART lock, for as long as they ran.
 *
 * Everything an app needs to read data, codes and freeze frames is untouched. */
bool ext_client_cmd_forbidden(const char *cmd, size_t len);

/* How many slots of the shared receive queue a client transport must leave free. That queue also
 * carries NC Flash's port-35001 traffic, and while a flash codec runs nothing drains it; a
 * client that filled it would make NC Flash's own receive task block. */
#define EXT_CLIENT_RXQ_RESERVE   (16U)

/* May a client transport push a chunk onto the shared receive queue? */
bool ext_client_rx_admit(bool flash_active, size_t queue_spaces);

/* Sleep: how long a client may hold off the low-voltage sleep countdown. An app left polling
 * with the engine off -- or anything on the LAN sending a byte now and then -- must not be able
 * to keep the device awake until the battery is flat. Half an hour covers reading codes with
 * the key on; after that the countdown starts whatever the client is doing. */
#define EXT_CLIENT_SLEEP_VETO_MAX_MS   (30U * 60U * 1000U)

/* `veto_ms` is how long the client has been the reason sleep was held off. */
bool ext_client_sleep_veto(bool client_active, uint32_t veto_ms);

#endif /* EXT_CLIENT_LOGIC_H */
