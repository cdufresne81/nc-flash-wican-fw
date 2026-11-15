# ELM327 clients — phone apps on port 35000

An ELM327 app (Torque Pro, Car Scanner, OBD Fusion…) connects over Wi-Fi to **TCP port 35000**
(`WICAN_ELM327_PORT`) and talks ELM327. The ESP32 does not interpret the protocol: it carries the
bytes to the on-board MIC3624 interpreter chip, which speaks ELM327 v2.3 itself, and carries the
answers back. This is the same route stock WiCAN PRO firmware uses.

The interesting part is not the route but the coexistence. The chip is a **second tester on the
same bus** as the datalogger (see "two paths to the CAN bus" in
[architecture.md](architecture.md)), and both ask the same ECU.

## The rule

Stated once, in `main/ext_client_logic.h`, and host-tested in `tools/hosttest/ext_client_test.c`:

1. **While an app is active, the datalogger is benched.** `can_ext_client_active()` is a term of
   `can_should_park()`, so `poll_log` parks at the top of its loop exactly as it does for an
   NC Flash pause.
2. **Active means "sent a byte within `EXT_CLIENT_HOLD_MS`"** (10 s). Nothing arms or releases
   the hold. An app that crashes, a phone that leaves, a socket that never closes: the logger is
   back ten seconds after the last byte. There is no lease to leak.
3. **NC Flash always wins.** While a flash codec, a host bus-claim, a host datalog pause or the
   sleep teardown holds the bus, `can_ext_client_refused()` is true and the app's command is
   answered `BUS BUSY` — the answer a real ELM327 gives when it cannot get the bus — without
   reaching the chip.

The hold is the weakest term in `can_should_park()`: it only benches the logger. It never keeps
NC Flash out, never blocks a reboot or a config save, and is not part of the flash fence.

## The route

| Step | Where | What happens |
|---|---|---|
| Accept | `comm_server.c`, `tcp_server_init()` | One client at a time. Bytes are tagged `DEV_WIFI` and pushed on the shared RX queue. |
| Route | `can_tx_task` in `main.c` | The `DEV_WIFI` arm: refuse, or touch the hold and call `elm327_process_cmd()`. |
| Send | `uart1_event_task` in `elm327.c` | Asks the gate again, then writes the command to the chip and reads until the `>` prompt, under the UART lock. |
| Reply | `send_to_host()` | Onto `xMsg_Tx_Queue`, drained by the `comm_server.c` TX task to the socket. |

`elm327_read_task` is **not** part of this and stays off. It forwards unsolicited chip output
(stock firmware's BLE path); running it for nobody is the 20-second wake bug described at
`elm327_set_read_task_enabled()`.

## Four things this path must not get wrong

**It must never block.** `can_tx_task` also carries NC Flash's port-35001 traffic, including the
fast-write commands. `elm327_process_cmd()` blocks forever on a full command queue, so the router
asks `elm327_cmd_queue_spaces()` first and answers `BUFFER FULL` when a chunk would not fit. A
stalled or flooding app cannot stall a flash.

**The gate is asked twice.** Once in the router, so a refused app does not even touch the hold;
and once in `uart1_event_task` at the moment of sending (`elm327_set_client_gate()`), which
catches commands that were already queued when NC Flash arrived. Only client commands are gated —
those queued with a reply queue. The firmware's own chip housekeeping passes none and always runs.

**Somebody has to drain TWAI.** With `poll_log` benched, the ESP32's controller is still on the
bus and still receives the ECU's answers to the chip's requests. `can_rx_task` drains and drops
them (`ext_client_drain`), so `poll_log` does not resume into a queue of stale `0x7E8` frames.
The drain still stamps `can_diag_idle_ms()`, which is the only view `can.c` has of chip traffic.

**Replies share one buffer.** `send_to_host()` copies through a single static buffer. Before
this feature only `can_tx_task` used it with a real queue; now `uart1_event_task` does too, for the
chip's answers. `s_send_to_host_mutex` serialises them so an app's reply cannot overwrite an
NC Flash reply.

## Sleep

A hold makes `poll_log_ecu_answering()` false, like any park. So the sleep state machine counts a
live hold as a live session (`ext_client_live` in `sleep_mode.c`): the low-voltage countdown does
not start, and a running one is cancelled, while an app is talking. Because the hold lapses on
its own, it cannot keep a parked car awake.

## EEPROM guard

The chip's "memory" function is on after every reset, and with it on the chip stores the protocol
it last answered on in non-volatile memory rated for roughly 10,000 writes. Apps reset the chip
(`ATZ`) at the start of every session and then search for a protocol. `uart1_event_task` already
turned `ATZ` into the lighter `ATWS`; it now also sends `ATM0` behind it and discards the answer.
The guard and its rationale come from upstream WiCAN v6 (meatpiHQ/wican-fw issue #1023).

## Observability

`GET /datalog` reports `ext_client_active` and `ext_client_idle_ms` (null until a client has been
seen). The serial log prints one line when a hold lapses and one per refused command.

## Not verified on hardware

This was written and host-tested without a device. Before relying on it, check on the bench:

- An app connects on 35000, initialises, and reads live data with the engine running.
- The trip CSV stops gaining rows within a second of the app's first command and resumes about
  ten seconds after the app is closed.
- Starting an NC Flash read while the app is polling: the app shows `BUS BUSY`, the read
  completes, and the app recovers afterwards.
- A sleep/wake cycle with an app left connected: the wake is not slowed (the UART lock is only
  held for the duration of one command, at most `ELM327_CMD_TIMEOUT_MS`).

## Known limits

- **One command can already be on the wire.** If NC Flash claims the bus while the chip is in the
  middle of a request, that one exchange finishes on the bus. The gate stops everything after it.
  NC Flash can read `ext_client_active` from `/datalog` and wait for the diagnostic-idle clock
  before starting.
- **The first app command may overlap one logger request**, because `poll_log` only parks at the
  top of its loop. Apps open with `AT` commands that do not touch the bus, so in practice the
  logger is parked before the first OBD request goes out.
- **One client.** `comm_server.c` serves a single socket; a second app waits.
- **Wi-Fi only.** BLE is a separate piece of work: `ble.c` still shuts Wi-Fi down when a client
  connects, and Torque Pro in particular expects Bluetooth Classic, which the ESP32-S3 lacks.
