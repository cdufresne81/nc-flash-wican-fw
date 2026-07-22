# csv_logger — the wide-CSV trip logger

`components/csv_logger/csv_logger.c` writes trip logs to the SD card as one **wide CSV**: one column per channel, one row per sample instant. Channels come from whatever protocol is producing records (`csv_logger_record(name, value, unit, source)`); the layout is designed so MegaLogViewer HD and similar tools get a dense rectangular table.

## Column layout

At session open, the column set is enumerated via a registered provider — `autopid_collect_log_columns` (in the autopid component), registered once at boot with `csv_logger_set_column_provider()`. The provider mirrors the producer gates exactly (std/custom/specific enables, `is_vehicle_specific && !pid_specific_en` skip), so every record that can be produced has a column and `cols_unmatched` stays 0. Channel sources are tagged `PID` / `STD` / `CANFLT` / `CALC`; the header only appends the source tag on **name collisions** (`dup_name`) — so don't grep the header for `[CANFLT]` to check hybrid capture, check the channel's column values.

## The fixed-rate grid

A grid timer ticks at a configured rate and each tick writes one full-width row of latest (LOCF) values — rows land at a steady spacing regardless of how records arrive; records themselves only refresh the LOCF snapshot. This is the **only** row-writing behavior: the per-record Event mode (`csv_grid_mode`, Task #11) was removed in issue #53, along with the config key and the UI "Polling Mode" dropdown. A stored `csv_grid_mode` key is ignored and drops out of `config.json` on the first Submit after the update.

`CSV_GRID_HZ_DEFAULT` is 10 Hz. The rate is latched per session at session open.

## The Auto (fastest) rate (issue #23)

`csv_grid_hz` is a string config key that accepts `1..100` **or `"auto"`**:

- `main/config_server.c` parses `"auto"` as a valid sentinel; the getter `config_server_get_csv_grid_hz()` then returns 1 with `*hz = 0` — **callers must treat `*hz == 0` as "auto"**, it is never a literal 0 Hz.
- At session open csv_logger latches `csv_grid_auto` from that sentinel. In auto mode, `csv_grid_period_ms()` re-derives the tick period *on every use* from the registered rate callback (`csv_logger_set_rate_fn()` — poll_log registers `poll_log_sweep_hz`), clamped to 10–1000 ms (100–1 Hz). If the callback is missing or returns 0 (rate not yet measured, or a protocol that never registers one), it falls back to `CSV_GRID_HZ_DEFAULT`.
- Net effect: the grid tracks the *measured* polled-sweep rate live. Bench-verified on `v1.6.0-2-ga4082fd`: 19-PID sweep measured 20.9 Hz → auto trip logged 402 rows in 19.8 s (20.2 Hz, 48–49 ms spacing, every polled column fresh each row).
- Since issue #29, `poll_log_sweep_hz()` reports the **fastest channel's** cadence, not the mean sweep — with per-PID `SampleEvery` divisors the two differ, and slaving the grid to the sweep would oversample every channel. Nothing here changed; the callback contract is the same float. See [poll_log.md](poll_log.md).
- **The 10 ms (100 Hz) clamp is the real ceiling, now aligned with the poll-log hard cap** (`POLLLOG_MIN_SWEEP_MS`, also 100 Hz — see [poll_log.md](poll_log.md)). Because the polled sweep is itself capped at 100 Hz, the auto grid can track it 1:1 all the way to the cap: the pre-#56 gap, where a fast config logged at 50 Hz while polling at up to 100 Hz, is closed. The clamp still bounds SD write rate and row width. Raised from 50 Hz in #56.

### Why slave the grid to the sweep

If the grid ticked faster than the data refreshed, channel values would repeat across rows (staircase rendering in log viewers). If slower, resolution is wasted. Matching the measured sweep gives the fastest rate at which every row still carries fresh polled values — the original "Tactrix logcfg" ask of issue #23, answered by measuring instead of estimating.

## Registration patterns (one-way dependencies)

csv_logger never links against protocol components. Producers register callbacks at init:

| Setter | Registered by | Purpose |
|---|---|---|
| `csv_logger_set_column_provider()` | autopid (boot) | Enumerate columns at session open |
| `csv_logger_set_engine_state_fn()` | poll_log | Suppress logging while engine off (quiesce) |
| `csv_logger_set_rate_fn()` | poll_log | Live rate for the Auto grid |

Adding a new protocol that should drive the Auto grid = implement a `float fn(void)` returning measured Hz (0 = unknown) and register it. Nothing else changes.

## RTC crash guard

A `RTC_NOINIT` guard protects against CSV-induced boot loops: a warm reboot within ~35 s of a CSV-armed boot (deferred start ~20 s + 15 s stable window) leaves the guard armed and **every subsequent warm reboot skips CSV**. Remote recovery without a cold power cycle: `POST /csv_logger?op=start` (manual path bypasses the guard), wait ~20 s for the writer to clear it, then `POST /system_reboot`. Practical rule: avoid back-to-back `/store_config` reboots on CSV-enabled devices.

## Manual trip control

`POST /csv_logger?op=start|stop` — starts/stops a session immediately (also the guard-recovery path above). Trip files are listed by `/csv_list` and fetched by `/download_csv`.
