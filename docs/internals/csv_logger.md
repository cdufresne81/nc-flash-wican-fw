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

If the grid ticked faster than the data refreshed, channel values would repeat across rows (staircase rendering in log viewers). If slower, resolution is wasted. Matching the measured rate gives the fastest grid at which the *fastest* channel is fresh on every row — the original "Tactrix logcfg" ask of issue #23, answered by measuring instead of estimating.

**With divisors, slower channels do staircase — by construction.** Before issue #29 every channel refreshed every sweep, so "fastest channel fresh on every row" meant *every* channel fresh on every row. That is no longer the same statement: the grid ticks at `sweep_ms × sched_min_n`, so a channel at `SampleEvery: N` produces a new value every `N / sched_min_n` rows and its cell is carried forward in between. A row is a rectangular snapshot, so the only alternatives are a blank cell or a ragged file, both of which log viewers handle worse than a held value. Asking for a channel to be sampled less **is** asking for it to repeat in the CSV; the grid is still the fastest one that wastes no resolution on the channels you did not gate. Mode-23 memory channels (issue #51) are the common case here, since they cost ~7× a Mode 01 channel and are usually worth a large `N` — see [poll_log.md](poll_log.md).

## Registration patterns (one-way dependencies)

csv_logger never links against protocol components. Producers register callbacks at init:

| Setter | Registered by | Purpose |
|---|---|---|
| `csv_logger_set_column_provider()` | autopid (boot) | Enumerate columns at session open |
| `csv_logger_set_engine_state_fn()` | poll_log | Suppress logging while engine off (quiesce) |
| `csv_logger_set_rate_fn()` | poll_log | Live rate for the Auto grid |

Adding a new protocol that should drive the Auto grid = implement a `float fn(void)` returning measured Hz (0 = unknown) and register it. Nothing else changes.

## RTC crash guard

A `RTC_NOINIT` guard protects against CSV-induced boot loops. `csv_bringup_decide()` owns the whole chain and is host-tested (`tools/hosttest/run.sh`); `csv_logger.c` only holds the two RTC words and acts on the answer.

The guard is armed immediately before a bring-up attempt and cleared only once the writer has run `CSV_GUARD_STABLE_US` (15 s). A reboot inside that window leaves it armed, so the next boot **skips** bring-up — correct, because the previous attempt genuinely did not survive.

What a skip costs is bounded two ways:

- The first consecutive skip schedules **one delayed retry** (`CSV_LOGGER_SKIP_RETRY_MS`, 60 s), so a spurious skip — a user reboot, a brownout — costs a minute rather than the whole drive.
- The second consecutive skip gives up until the next boot (`CSV_BRINGUP_MAX_SKIPS`). That bound is what the guard is *for*: a deterministic init crash runs START → SKIP_RETRY → SKIP_FINAL and stops.

`csv_skip_count` only resets when the writer proves stable, or on a boot that starts normally. Both RTC words are cleared in exactly one place — the writer's stability block — which also clears `csv_bringup_skipped`, re-enabling sleep resume-in-place (`sleep_mode_recovery_needed()`).

**This was a field failure, not a hypothetical.** RTC memory survives a software reboot but not a power cut, so before the retry existed a device whose guard got armed by an unlucky reboot could not be recovered by rebooting — only by physically unplugging the dongle. It was also silent: the skip logged at `ESP_LOGW`, which `esp_log_level_set("*", ESP_LOG_NONE)` discards whenever `debug` is off. Both are fixed: the skip now emits an `EVL_WARN` event-log line and `/csv_status` reports it.

Recovery paths, in order of preference: wait 60 s for the retry; `POST /csv_logger?op=start` (the manual path never arms the guard); or power-cycle.

## Auto-start visibility

The writer task starts at boot with **no delay**, and the first file opens when the ECU actually answers — ~3–5 s, not 20+.

There used to be a 20 s wait before the writer task was even created, with no status reported meanwhile, so "still starting" and "broken" were indistinguishable. Nothing replaced it: `csv_logger_start_at_boot()` runs *before* `autopid_init()`/`poll_log_init()`, no record can exist until a producer starts, and no session opens until a record arrives — the ordering is the margin. (`event_log`'s writer has always done SD `fopen`/`fwrite`/`fsync` from t=0 on the same card with no holdoff, which is the evidence a clock here bought nothing.) Removing it also means the crash guard's 15 s stability window starts at boot instead of 20 s in, halving the window a reboot can land inside.

`/csv_status` reports the state so the UI never has to guess:

| field | meaning |
|---|---|
| `autostart` | `disabled` \| `skipped_retry` \| `skipped` \| `unavailable` |
| `autostart_in_ms` | ms left on the retry countdown, 0 when nothing is counting |
| `bringup_skipped` | the raw `csv_logger_bringup_skipped()` the sleep path reads |

`csvAutostartLabel()` (main.js) turns those into the recorder card's line; before it existed, "still starting", "skipped" and "broken" all rendered as **Idle**.

## Manual trip control

`POST /csv_logger?op=start|stop` — starts/stops a session immediately (also the guard-recovery path above). Trip files are listed by `/csv_list` and fetched by `/download_csv`.

**Stop is per-trip.** `CSV_MANUAL_OFF` is cleared back to `CSV_MANUAL_AUTO` on the next debounced ignition-off edge (`csv_manual_mode_next()`), so the following key-on records normally. It used to hold until a reboot, which meant one press of Stop silently disabled auto-logging for every subsequent trip. `CSV_MANUAL_ON` is deliberately *not* cleared that way — bench work needs FORCE_ON to survive a voltage flapping across the ignition threshold. The rearm never fires while a host holds the datalog park lease (`can_datalog_park_active()`): un-parking belongs to `datalog_restore_mode()` alone.
