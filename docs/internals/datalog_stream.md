# datalog_stream — live tail of the trip CSV (issue #3)

`main/datalog_stream.c` serves a read-only TCP copy of the trip CSV on **port 35002**
(`WICAN_DATALOG_STREAM_PORT`), so a PC tool (NC Flash, then MegaLogViewer tailing its file) can
follow a drive live. The SD file stays the complete record; the stream is a view of it. The wire
protocol (`NCDLv1`) is documented at the top of `main/datalog_stream.h`.

## Where the lines come from

`csv_logger` calls a registered `csv_stream_hook_t` (`csv_logger_set_stream_hook`) from the
writer task only, at four points: `CSV_STREAM_EV_OPEN` (end of `csv_open_new_file`, with the
header as one line), `CSV_STREAM_EV_ROW` (after the row `fprintf` in `csv_emit_wide_row`),
`CSV_STREAM_EV_CLOSE` (in `csv_close_file`) and `CSV_STREAM_EV_POLL` (once per writer-loop pass).
The SD writes themselves are unchanged; the header is copied into `csv_line_buf`, which is free
until the first row, as it is written.

## One producer, one consumer, no lock

- The **writer task** (`stream_hook`) is the only producer. It owns the trip state, keeps a copy
  of the header, and decides every line, including the sync a newly connected client needs
  (`#session` + header, or `#idle`). It never blocks: a message fits in the ring or is dropped.
- The **stream task** (`stream_task`, priority 3, below the writer's 4) is the only consumer and
  the only code that touches a socket.
- The one shared value is `s_live_gen`: 0 means no client, otherwise it changes on every
  connection. Each message carries the generation it was written for; the stream task discards
  the rest. That is what stops rows queued for a previous client leaking into a new one.

A session boundary is never dropped: if the sync does not fit, rows are dropped (and counted as
`#drop n`) until it does, so a row can never be read as a header.

## Memory: PSRAM, on purpose

The first version (parked branch `feature/live-datalog-stream`, July 2026) held a 16 KB ring in
internal RAM from boot. That left the trip logger unable to allocate its 22.5 KB queue, and
starting a trip returned *"CSV start failed (out of memory)"*. This version puts the ring (32 KB),
the header copy and both message buffers in PSRAM, allocated once by `datalog_stream_init()`.
Internal RAM cost is a few hundred bytes of `.bss`.

PSRAM is safe here because only two ordinary tasks touch these buffers, and no ordinary task runs
while the flash cache is off. lwIP already keeps its buffers in PSRAM on this build
(`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`), so streamed bytes pass through PSRAM regardless.

## Clients

One client at a time, and **the newest wins**: a new connection closes the old one, after telling
it `#bye replaced`. A PC that reconnects after a Wi-Fi drop therefore gets straight back in,
instead of waiting for keepalive to notice its dead connection. A client told `#bye replaced` must
not reconnect, or two clients take turns forever (each swap writes two event-log lines to the SD).
A host must not probe 35002 while it is streaming, or it kicks itself. Use `GET /check_status`
(`stream_port`) to detect support.

After 3 s with nothing to send, the stream sends `#ping`. The client never sends, so without it a
client could not tell a quiet stream from a dead one: when the device sleeps (Wi-Fi torn down) or
reboots, the reset never reaches the PC. A client should treat ~15 s of silence as a dead link.

The stream task polls a non-blocking listener and `recv(MSG_DONTWAIT)` between messages rather
than calling `select()`: the VFS `select()` allocates from the internal heap on every call, and
this loop runs 20+ times a second while a client is connected.

A client that stops reading is dropped after `STREAM_SEND_TIMEOUT_S`. Connect and disconnect
(with the reason and the row counts) go to the event log.

`/check_status` carries `stream_port`, `stream_connected`, `stream_rows_sent` and
`stream_rows_dropped`.

## What it does not touch

No CAN bit, no park/claim lease, no flash path, no config key. A connected stream client is not
"host present" for the dead-man reaper. During an ECU flash the producers are parked, so no rows
flow and the stream is silent.

`CONFIG_LWIP_MAX_SOCKETS` went from 18 to 22 and `CONFIG_LWIP_MAX_ACTIVE_TCP` from 16 to 20 with
this feature: the web server alone may hold 15 connections, plus 35001 and a stream client. At the
TCP cap new connections are refused (a 35001 connect, an OTA upload), existing ones are never cut.

Side effect worth knowing: the lwIP socket table lives in PSRAM `.bss`, so growing it moved
`_ext_ram_noinit_start`. The restart tracker's state (`EXT_RAM_NOINIT_ATTR`) then fails its
magic check after the OTA and starts over: `restart_boot_count` resets and the first boot reports
no `ota_apply` reason. Benign, and any future change to PSRAM `.bss` size does the same.
