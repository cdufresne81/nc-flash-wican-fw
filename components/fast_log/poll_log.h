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
 */

#ifndef POLL_LOG_H
#define POLL_LOG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * POLL_LOG mode (Task #18, Phase B "measure-first"): native-TWAI request/response
 * polling of the configured mode-01/22 PIDs, with NO ELM327 emulation and NO hardcoded
 * 100 ms inter-poll delay. Single-PID round-robin -- the simplest correct poller -- so we
 * can measure the REAL per-request turnaround on the PCM before adding batching.
 *
 * Sibling to fast_log (passive broadcast). Selected via the "poll_log" protocol string.
 */
void poll_log_init(char *id, uint32_t log_period);

/* True when the RTC crash-guard made poll_log_init() skip bring-up on this boot. Stays true for
 * the whole uptime. The sleep path ORs this with the other bring-up guards and takes the reboot
 * fallback instead of resuming in place, so the "retry on the next boot" that the guard promises
 * still happens on a device that no longer reboots to wake. */
bool poll_log_bringup_skipped(void);

/*
 * Live poll metrics for GET /poll_status, as a malloc'd JSON string the caller must free().
 * Safe to call in any protocol mode -- returns {"active":false,...} when POLL_LOG isn't running.
 * Shape: {active, ok, timeout, txfail (cumulative), rtt_avg_ms/min/max, req_s (last 3 s window),
 * sweep_ms/sweep_hz (measured full-sweep EMA), pids (polled PID count),
 * win_ok/win_timeout/win_txfail (that window's counts)}.
 */
char *poll_log_get_status_json(void);

/*
 * Measured full round-robin sweep rate in Hz (EMA), i.e. the fastest rate at which every polled
 * channel delivers a fresh value (issue #23). 0 when POLL_LOG is inactive or nothing has been
 * measured yet -- callers fall back to their own default. Registered with the CSV logger as its
 * rate provider (csv_logger_set_rate_fn) to drive the "Auto" fixed-rate grid.
 */
float poll_log_sweep_hz(void);

/*
 * Engine/quiesce state, derived purely from whether the ECU is answering polls (Stage 1).
 * Safe to call in any protocol mode: when POLL_LOG is not the active mode they report
 * "engine running / bus not idle" so other modes and stale reads never suppress logging.
 *  - poll_log_engine_running(): true while the ECU is answering (or POLL_LOG inactive).
 *  - poll_log_quiesced():       true while the bus is flipped to LISTEN_ONLY (engine off).
 *  - poll_log_bus_idle_ms():    ms since the last received frame while quiesced; UINT32_MAX
 *                               while actively polling / outside POLL_LOG (Route-B sleep sensor).
 */
bool     poll_log_engine_running(void);
bool     poll_log_quiesced(void);
/* The RECORDING gate: true while the conditions to log are met and the sweep runs at full rate.
 * Different question from poll_log_engine_running() -- the ECU answers at key-on with the engine
 * off. This is what the CSV logger gates on. True when POLL_LOG is not the active mode. */
bool     poll_log_gate_open(void);
uint32_t poll_log_bus_idle_ms(void);

/*
 * Request a live PID-table hot-swap (issue #39). Called on the httpd task after
 * auto_pid.json is rewritten; sets a flag only -- the re-parse + atomic swap runs on the
 * poll task at its safe point (deferred while a CSV trip is open). Returns true if queued,
 * false when POLL_LOG isn't the active mode (caller must then require a reboot instead).
 */
bool     poll_log_request_reload(void);

#endif /* POLL_LOG_H */
