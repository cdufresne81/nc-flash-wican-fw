# ELM327 clients — phone apps on port 35000 and over BLE

An ELM327 app (Torque Pro, Car Scanner, OBD Fusion…) connects over Wi-Fi to **TCP port 35000**
(`WICAN_ELM327_PORT`) and talks ELM327. The ESP32 does not interpret the protocol: it carries the
bytes to the on-board MIC3624 interpreter chip, which speaks ELM327 v2.3 itself, and carries the
answers back. This is the same route stock WiCAN PRO firmware uses.

With BLE enabled in Settings, the same apps can connect over BLE instead: writes to `FFF2` of
the `FFF0` service take the same route, and answers come back as notifications on `FFF1`.

The interesting part is not the route but the coexistence. The chip is a **second tester on the
same bus** as the datalogger (see "two paths to the CAN bus" in
[architecture.md](architecture.md)), and both ask the same ECU.

## The rule

Stated once, in `main/ext_client_logic.h`, and host-tested in `tools/hosttest/ext_client_test.c`:

1. **While a client is active, the datalogger is benched.** `can_ext_client_active()` is a term
   of `can_should_park()`, so `poll_log` parks at the top of its loop exactly as it does for an
   NC Flash pause.
2. **Active means "a client byte arrived, or a client command reached the chip, within
   `EXT_CLIENT_HOLD_MS`"** (10 s). Nothing arms or releases the hold. An app that crashes, a
   phone that leaves, a socket that never closes: the logger is back ten seconds later. There is
   no lease to leak.
3. **NC Flash always wins.** While a flash codec, a host bus-claim, a host datalog pause or the
   sleep teardown holds the bus, `can_ext_client_refused()` is true and a client command is
   answered `BUS BUSY` — the answer a real ELM327 gives when it cannot get the bus — without
   reaching the chip.

The hold is the weakest term in `can_should_park()`: it only benches the logger. It never keeps
NC Flash out, never blocks a reboot or a config save, and is not part of the flash fence.

## Who is a client

Every command queued for the chip is a client command **unless the firmware queued it for its own
housekeeping** with `elm327_process_housekeeping_cmd()`. Only `obd.c` does: power-pin setup,
sleep configuration, the battery-voltage read. So the 35000 socket, BLE and the wired UART
(`wc_uart.c`) are all clients and all gated.

The test is who called, not what the arguments look like. The first version keyed on "has a reply
queue" and let the wired UART and Test-PID through during a flash.

## The route

| Step | Where | What happens |
|---|---|---|
| Accept (Wi-Fi) | `comm_server.c`, `tcp_server_init()` | One client at a time. Bytes are tagged `DEV_WIFI` and pushed on the shared RX queue, if `can_ext_client_rx_admit()` allows. |
| Accept (BLE) | `gatts_profile_event_handler()` in `ble.c` | Writes to the `FFF2` value only, on an encrypted, passkey-paired link, tagged `DEV_BLE`, same admission test. |
| Route | `elm327_client_route()` in `main.c` | Refuse, or touch the hold and call `elm327_process_cmd()`. One `elm327_client_t` per transport, so each has its own half-typed line and its own reply queue. |
| Send | `uart1_event_task` in `elm327.c` | Applies the hooks from `elm327_set_client_ops()`, writes the command to the chip and reads until the `>` prompt, under the UART lock. |
| Reply | `elm327_client_reply()` | Onto the transport's queue: `xMsg_Tx_Queue` (drained by the `comm_server.c` TX task) or `xmsg_ble_tx_queue` (drained by `ble_task`). |

`elm327_read_task` is **not** part of this and stays off. It forwards unsolicited chip output and
was stock firmware's BLE path (raw `elm327_send_cmd()` in, the read task out). BLE now uses the
request/response worker like TCP does, which is what puts it behind the gate; running the read
task for nobody is the 20-second wake bug described at `elm327_set_read_task_enabled()`.

## Keeping a client away from a flash

This is the part that matters, and it is several separate mechanisms because there are several
separate ways a phone app could reach into an NC Flash session.

**The gate is asked three times.** In the router, so a refused app does not touch the hold. In
`uart1_event_task` when the command comes off the queue, which catches commands that were queued
before NC Flash arrived. And again once the UART lock is held, because that wait can be long.

**A running command is cut short.** The reply loop polls every `ELM327_CMD_POLL_MS`. When the bus
becomes reserved it stops waiting, interrupts the chip (a space: the chip ignores spaces, whereas
a carriage return on an idle chip repeats the last command), discards what is left, and answers
`BUS BUSY`.

**The flash side waits for that.** `can_flash_active_set()`, `can_host_bus_claim_arm()` and
`can_park_lease_arm()` raise their flag and then wait in `ext_client_quiesce()` until
`elm327_client_cmd_in_flight()` drops, up to `EXT_CLIENT_QUIESCE_MAX_MS`. With no app connected
the wait is zero. The order — raise, then look — is what makes it race-free against the worker,
which marks itself in flight before it asks the gate.

**The router never blocks.** `can_tx_task` also carries port 35001, including the fast-write
commands. Its own answers (`elm327_client_say()`) are sent without waiting, a chunk that would
not fit in the chip's command queue is answered `BUFFER FULL`, and the enqueue itself is bounded
(`ELM327_CMD_ENQUEUE_WAIT_MS`).

**The shared RX queue keeps room for NC Flash.** That queue carries 35001 too, and nothing
drains it while a codec runs. A client transport drops its chunk when a flash is active or when
fewer than `EXT_CLIENT_RXQ_RESERVE` slots are free, and never waits on it.

**Replies do not share a buffer.** `send_to_host()` copies through one static buffer and is used
by `can_tx_task` only. The chip's answers go through `elm327_client_reply()`, which has its own.

## Keeping a client from harming the chip

**Forbidden commands.** `ext_client_cmd_forbidden()` refuses, with `?`, two families: commands
that change what the chip remembers or how the firmware reaches it (`AT PP`, baud rate, sleep
configuration, NVM reset), and bus monitors (`AT MA` and relatives), which stream without ever
printing a prompt. The check runs in `uart1_event_task`, the only place the whole line is in one
piece.

**The reply is bounded.** The reply loop has a wall-clock deadline of `ELM327_CMD_TIMEOUT_MS`
for the whole answer and stops when its buffer is full. Before, the timeout was the gap allowed
between chunks and reads were appended with no bound, which only the firmware's own well-behaved
commands could reach. A reply that is cut short is answered `STOPPED`.

**EEPROM guard.** The chip's "memory" function is on after every reset, and with it on the chip
stores the protocol it last answered on in non-volatile memory rated for roughly 10,000 writes.
Apps reset the chip (`ATZ`) at the start of every session and then search for a protocol.
`uart1_event_task` already turned `ATZ` into the lighter `ATWS`; it now also sends `ATM0` behind
it and discards the answer. The guard comes from upstream WiCAN v6 (meatpiHQ/wican-fw issue #1023).

## What the datalogger does meanwhile

**It stops writing rows.** The poller is parked, so every column would repeat its last value. The
CSV writer's fixed-rate grid skips rows while `can_ext_client_active()`; the file stays open and
the log has a gap in its timestamps.

**Somebody drains TWAI.** The ESP32's controller is still on the bus and still receives the
ECU's answers to the chip's requests. `can_rx_task` drains and drops them (`ext_client_drain`)
with `can_receive_nb()`, re-asking on every frame whether the hold is still in force, so
`poll_log` does not resume into a queue of stale `0x7E8` frames. The drain still stamps
`can_diag_idle_ms()`, which is the only view `can.c` has of chip traffic.

## BLE and Wi-Fi together

Stock firmware called `config_server_stop()` and `wifi_mgr_disable()` on every BLE connect and
undid both on disconnect, so a phone pairing over BLE cut NC Flash and the web console off. Both
calls are gone from `ble.c`. The radios share the antenna through software coexistence
(`CONFIG_ESP_COEX_SW_COEXIST_ENABLE`), and what a BLE app may do is decided by the bus rule above,
the same as for a TCP app.

What did not change: the stack is still Bluedroid, pairing is still passkey with MITM protection
and bonding, the `FFF2` write characteristic still requires an encrypted link, and BLE is still
off unless `ble_status` is `enable`. The BLE settings are visible in the web UI again and are no
longer forced off in AP+Station mode.

Two costs to know about. With BLE enabled, `sleep_mode_resume()` refuses to resume in place and
every wake is a reboot, because re-initialising the BT controller in place crashed the device.
And BLE takes internal RAM (about 15 KB by the July 2026 audit) on top of the interpreter-chip
path, on a build whose tight resource is internal RAM; the headroom with both in use has not been
measured.

## Sleep

A hold makes `poll_log_ecu_answering()` false, like any park. So the sleep state machine counts a
live hold as a live session (`ext_client_live` in `sleep_mode.c`): the low-voltage countdown does
not start, and a running one is cancelled, while an app is talking.

Unlike the NC Flash terms this one is capped. `ext_client_sleep_veto()` withdraws it once a
client has been continuously active for `EXT_CLIENT_SLEEP_VETO_MAX_MS` (30 minutes), so an app
left open on a phone, or anything on the LAN poking port 35000, cannot hold a parked car awake.

## Observability

`GET /datalog` reports `ext_client_active` and `ext_client_idle_ms` (null until a client has been
seen). The serial log prints one line when a hold lapses, one per refused or cut-short command,
and one when a flash had to wait for the chip.

## Not verified on hardware

This was written, host-tested and reviewed without a device. Before relying on it, check on the
bench:

- An app connects on 35000, initialises, and reads live data with the engine running.
- The trip CSV stops gaining rows within a second of the app's first command and resumes about
  ten seconds after the app is closed.
- Starting an NC Flash read and a flash while the app is polling: the app shows `BUS BUSY`, the
  operation completes, and the app recovers afterwards. This is the one that matters most.
- The interrupt works as assumed: a space sent to the chip mid-request stops it, and does not
  disturb its next command.
- A sleep/wake cycle with an app left connected: the wake is not slowed.
- With BLE enabled: a phone pairs with the passkey, a BLE ELM327 app reads live data, and during
  that the web console and an NC Flash connection on 35001 stay reachable.
- Free internal heap (`/check_status`) with a BLE app connected and polling, and a trip recording
  started right after it disconnects.
- A sleep/wake cycle with BLE enabled comes back by reboot, and BLE is advertising again.

## Known limits

- **A request the chip has already transmitted cannot be recalled.** The quiesce stops the chip
  waiting and stops anything further, but if the chip had just sent a request, the ECU's answer
  to it may still arrive as NC Flash begins. The codecs drain TWAI before their first exchange.
- **The first app command may overlap one logger request**, because `poll_log` only parks at the
  top of its loop. Apps open with `AT` commands that do not touch the bus, so in practice the
  logger is parked before the first OBD request goes out.
- **BLE stays up during a flash.** The radios share one antenna, and BLE traffic during a
  fast-read or a flash takes airtime from the Wi-Fi link NC Flash is using. Unmeasured; until it
  is, leave BLE disabled on a device used for flashing, or keep phones disconnected while flashing.
- **Port 35000 is open to the network the device is on**, as 35001 is. Anyone on it can bench
  the logger by sending bytes.
- **One client per transport.** `comm_server.c` serves a single socket; a second app waits.
- **Torque Pro is Wi-Fi only here.** It expects Bluetooth Classic, which the ESP32-S3 lacks.
  Apps that support BLE adapters with the `FFF0` service (Car Scanner, OBD Fusion) are the ones
  the BLE path is for. Neither has been tried against this build.
- **Still Bluedroid.** Moving to NimBLE would roughly halve BLE's footprint; it is a rewrite of
  `ble.c` and was left for after this path is proven on a device.
