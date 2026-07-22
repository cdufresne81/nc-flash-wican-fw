# Goal: eliminate the `interrupt_wdt` reset under heavy CSV SD logging by coalescing SD writes

**`/goal` condition (paste-ready):** *Implement the sector-aligned SD write accumulator in the CSV logger per this document until all acceptance criteria in section 4 hold. The manual checklist in section 6 is explicitly OUTSIDE the goal condition.*

Target: WiCAN OBD-PRO, ESP32-S3, ESP-IDF v5.5.3. Base branch: `wican-pro`. Do the work on a dedicated branch (e.g. `fix/interrupt-wdt-sd-coalesce`).

---

## 1. Goal

The device takes an `interrupt_wdt` reset during CSV logging under SD write pressure. It was captured live and decoded (see [[crash-report-plan]] / `components/crash_report`): reset reason `interrupt_wdt`, core 0, backtrace innermost-first `_xt_lowint1 -> i2c_isr_handler_default -> vPortExitCritical -> xQueueReceive -> sdmmc_host_wait_for_event -> sdmmc_wait_for_idle -> sdmmc_write_sectors -> f_write -> vfs_fat_write`. Reproduction is **rate-scaled**: grid=100 (~81 Hz rows) resets in ~20 s; ~43 Hz ran 16 min clean.

Fix the crash **at its cause** (the SD write-transaction rate), on the stock watchdog, without masking, relocating tasks, or reducing the logging feature.

## 2. Root cause (grounded)

- The INT WDT fires when a core's FreeRTOS **tick** is starved past `CONFIG_ESP_INT_WDT_TIMEOUT_MS=300` (`sdkconfig:1704`); `CONFIG_ESP_INT_WDT_CHECK_CPU1=y` (`sdkconfig:1705`) means starving **either** core is fatal (`int_wdt.c:106-127`). So core-pinning cannot help.
- The IDF v5.5.3 sdmmc driver does **not** hold interrupts off waiting for the card: the wait is a blocking `xQueueReceive` (`sdmmc_host.c:977`) under a **mutex** (`sdmmc_transaction.c:105`), busy-poll yields via `vTaskDelay(1)` after 100 ms (`sdmmc_common.c:436-447`). So there is no single long interrupts-off window; the tick is starved by the **rate** of SD-write transactions and their level-1 sdmmc-ISR storm (empirically confirmed by the 81 Hz vs 43 Hz split; the exact tick-starvation amplifier — cross-core spinlock spin at EXCM, cache-writeback stall — is **not** fully isolated and should not be asserted as proven).
- The rate driver is the CSV write pattern: the writer emits one wide ~150-190 B row per grid tick via a single `fprintf` (`csv_logger.c:436`). There is **no `setvbuf` in the repo**, so the FILE* uses **newlib**'s default `BUFSIZ=128` (libc is `CONFIG_LIBC_NEWLIB=y`, `sdkconfig:2414`). Each row overflows 128 B, so every row reaches `f_write`, which issues a **single-block** `disk_write` per sector crossing -> ~27 single-block SD transactions/s at 81 Hz vs ~14/s at 43 Hz, plus the 1 Hz `fflush`+`fsync` (`csv_logger.c:749-750`) and the idle-pass `fsync` (`csv_logger.c:672-673`).
- The `i2c_isr_handler_default` frame is an incidental level-1 co-tenant on core 0, **not** causal.

## 3. Design — sector-aligned internal-DMA write accumulator

Coalesce the per-row `fprintf` stream into **whole 512-byte SD sectors**, so each physical write is one **multi-block** transfer (`MMC_WRITE_BLOCK_MULTIPLE`, `sdmmc_cmd.c:511-512`) instead of ~3 single-block ones. Determinism is the whole point: bare `setvbuf` is bimodal (~16x fewer transactions only when the file offset is 512-aligned, ~1x when not), and the periodic `fsync` flushes partial buffers that poison alignment — so it can pass a bench run by luck and regress in the field. The accumulator removes that non-determinism.

All changes are in `components/csv_logger/csv_logger.c`.

1. **Persistent aligned buffer.** Allocate once, reuse across sessions: `heap_caps_malloc(CSV_SD_ACC_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)`, `CSV_SD_ACC_BYTES` a 512 multiple (start 8192), 4-byte aligned. **Internal RAM is mandatory** — the S3 has no `SOC_SDMMC_PSRAM_DMA_CAPABLE` (a PSRAM source forces the per-block bounce loop, `sdmmc_cmd.c:463-490`) and the writer dereferences it inside `fprintf`/`fflush` flash-cache-disable windows (existing brick-safety discipline, `csv_logger.c:126`; mirrors `csv_line_buf` at `csv_logger.c:131/1376`). Track a `size_t acc_len` (bytes currently held, always `< 512` after a drain of the whole-sector prefix... see below).
2. **Append instead of `fprintf`-to-FILE.** The row builder already formats one row into `csv_line_buf` before the single `fprintf` at `csv_logger.c:436`. Change the sink: append those bytes into the accumulator. When `acc_len >= 512`, write the largest 512-multiple prefix in ONE call (`fwrite(acc, 1, prefix, csv_file)` — a 512-multiple from a 4-aligned internal buffer takes the multi-block DMA path, `ff.c:4112-4117`), then `memmove` the `< 512` remainder to the front. To keep the FILE offset deterministically sector-aligned, make the FILE* unbuffered (`setvbuf(csv_file, NULL, _IONBF, 0)` right after `fopen`, `csv_logger.c:243`) so stdio never re-buffers/re-fragments what the accumulator hands it. The file starts at offset 0 and only ever advances by 512-multiples -> offset stays sector-aligned for the whole session.
3. **Checkpoint = flush whole sectors, keep the tail.** Replace the semantics of the 1 Hz flush (`csv_logger.c:749-750`) and the idle-pass `fsync` (`csv_logger.c:672-673`): a checkpoint writes the current whole-512 prefix then `fsync`s, and **retains** the `< 512` B tail in the accumulator (do NOT `fwrite` a partial sector mid-session — that is what misaligns the offset). This keeps 512-alignment while still `fsync`ing at the existing 1 Hz cadence, so worst-case abrupt-power-loss stays bounded by the checkpoint period (~1 s of rows, **unchanged from current firmware**, which already `fsync`s at 1 Hz); the `< 512` B tail is the always-in-RAM residue, **not** the loss bound. Tighter durability = shorter checkpoint period = more transactions (the dial back toward the crash), so keep it at 1 s. Every streaming write is still multi-block.
4. **Graceful close stays lossless.** On stop/rotate (`csv_logger.c:285-287`): `fwrite` the final partial tail, then `fflush`+`fsync`+`fclose` as today. A graceful ignition-off loses nothing. On rotation reopen, reset `acc_len=0` (new file at offset 0 -> aligned again).

Keep `CSV_LOGGER_FLUSH_PERIOD_MS` (`csv_logger.c:72`) as the checkpoint cadence; it may be raised, but the tail-retention already removes the partial-sector-write cost, so raising it is optional, not required.

## 4. Acceptance criteria (each verifiable from the session transcript)

The `/goal` evaluator cannot run commands; each check below is demonstrated in Claude's own output.

1. **Builds clean.** `idf.py build` exits 0 on the fix branch; show the "Project build complete" line and the generated `.bin`.
2. **Buffer is internal-DMA, writes are sector-aligned.** `grep` shows the accumulator alloc uses `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA` (never `MALLOC_CAP_SPIRAM`) and that the streaming `fwrite` length is a 512 multiple; `grep` shows the `setvbuf(..., _IONBF, ...)` on `csv_file` and that no partial-sector `fwrite` happens on the periodic checkpoint path.
3. **Baseline crash reproduced (gate is real).** On the pre-fix build, grid=100 (~81 Hz) CSV logging produces an `interrupt_wdt` reset within a few minutes — shown via `/check_status` `restart_unexpected_reset_count` incrementing and a `/event_log` `CRASH INT_WDT` line. (Already demonstrated once at ~20 s.)
4. **Mechanism moved, not just the symptom (HARD gate).** With the fix, instrumentation (a temporary counter/log around `disk_write` / `sdmmc_write_sectors`, or the sdmmc block-count) demonstrates streaming writes now take the **multi-block** path (transfer count > 1 sector) and that single-block transactions/s dropped materially vs the section-3 baseline. Shown in captured output.
5. **Survives the repro on the stock watchdog.** With `CONFIG_ESP_INT_WDT_TIMEOUT_MS` back at 300, grid=100 (~81 Hz) sustains **>= 30 min** with `restart_unexpected_reset_count` unchanged and no new `/event_log` `CRASH` line. Shown via periodic `/check_status` + `/event_log` polls. (A temporary 300->800 bump at `sdkconfig:1704` + mirror `:3069` is permitted DURING bring-up so a reset does not cut telemetry; it MUST be reverted before this criterion is claimed.)
6. **Stock ceiling characterized.** grid set to the shipping max (50 Hz) runs a defined window (>= 20 min) and the result (crash or clean) is reported, so we know whether stock firmware needs this fix at all.
7. **Data integrity.** A CSV downloaded mid-session and one after a graceful stop each parse: correct header row, monotonic `timestamp_ms`, expected column count, no truncation; graceful-stop row count matches rows emitted (zero loss). Shown via fetched file stats.
8. **No coexistence regression.** During logging, SLCAN (TCP 35001) connects and HTTP `/check_status` responds; `/poll_status` shows no new timeouts vs baseline. Shown via the respective responses.

## 5. Constraints — what must NOT change

- **No task relocation across cores** and **no disabling `CONFIG_ESP_INT_WDT_CHECK_CPU1`** — both are defeated/dangerous (masks core-1 lockups on an unattended logger).
- **No permanent `CONFIG_ESP_INT_WDT_TIMEOUT_MS` increase** in the shipped build (bench backstop only, reverted per criterion 5).
- **No reduction of the grid-rate ceiling** (`WICAN_LOG_MAX_HZ` / clamps at `csv_logger.c:544,711`, `config_server.c:2842,3636`) — the shipping cap already limits rate; do not shrink the feature.
- **Accumulator stays in INTERNAL RAM** (never PSRAM) — brick-safety + DMA-capability.
- **Graceful-stop data loss must stay zero** (on-close flush at `csv_logger.c:285-287` intact); abrupt-power-loss window must be **no worse than current firmware** (~1 s, the checkpoint period) — do not widen the `fsync` cadence to buy alignment.
- **CSV format unchanged** — same columns, header, and LOCF semantics; do not touch the poll_log path, the RTC crash guard, or `components/crash_report`.
- **Internal-RAM budget respected** — do not regress boot/heap; measure `MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA` free headroom under peak load (WiFi+BLE+TLS) and drop `CSV_SD_ACC_BYTES` to 4096 if 8192 is tight.

## 6. Manual checklist (physical — OUTSIDE the `/goal` condition)

- Visually confirm the datalog activity LED blinks throughout a logging session (I2C path unaffected).
- After an **abrupt power cut** mid-log, confirm the SD card mounts cleanly on a host PC and the last file is intact up to its last checkpoint (power-loss durability is not scriptable on-device).
- Optional: logic-analyzer / bus trace confirming multi-block `CMD25` on the SD bus during streaming writes (belt-and-suspenders for criterion 4).

---

*Design basis: the `interrupt-wdt-fix-design` workflow (investigate/design/verify/synthesize) + independent Fable review (ENDORSE-WITH-CHANGES: build around the deterministic aligned accumulator, not bare setvbuf; newlib not picolibc; make the multi-block mechanism check a hard gate; characterize 50 Hz). Full design + review saved in the session scratchpad.*
