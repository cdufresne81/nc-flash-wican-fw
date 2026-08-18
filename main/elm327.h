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


#ifndef __ELM327__
#define __ELM327__

#include "driver/twai.h"

#define OBD_FW_VER_V18      "V2.3.18"
#define OBD_FW_VER_V22		"V2.3.22"

typedef void (*response_callback_t)(char*, uint32_t, QueueHandle_t *q, char* cmd_str);

#define ELM327_CAN_RX   0x01
#define ELM327_CAN_TX   0x02

typedef enum{
	ELM327_READY,
    ELM327_SLEEP
}elm327_chip_status_t;

void elm327_init(response_callback_t rsp_callback, QueueHandle_t *rx_queue, void (*can_log)(twai_message_t* frame, uint8_t type));

#if HARDWARE_VER == WICAN_PRO
int8_t elm327_process_cmd(uint8_t *buf, uint32_t len, QueueHandle_t *q, char *cmd_buffer, uint32_t *cmd_buffer_len, int64_t *last_cmd_time, response_callback_t response_callback);
elm327_chip_status_t elm327_chip_get_status(void);
esp_err_t elm327_update_obd(bool force_update);
#else
int8_t elm327_process_cmd(uint8_t *buf, uint8_t len, twai_message_t *frame, QueueHandle_t *q);
#endif

void elm327_run_command(char* command, uint32_t command_len, uint32_t timeout, QueueHandle_t *response_q, response_callback_t response_callback, bool stop_after_first_frame, uint32_t expected_frame_id);
esp_err_t elm327_sleep(void);
void elm327_lock(void);
void elm327_send_cmd(uint8_t* cmd, uint32_t cmd_len);
esp_err_t elm327_get_protocol_number(uint8_t *protocol_number);
void elm327_hardreset_chip(void);

/* Same hard reset, but the wait for the shared UART lock is bounded by the caller instead of the
 * fixed ELM327_CMD_MUTEX_TIMOUT (10 s). Returns true when the chip is talking again.
 *
 * This exists because of a measured 20 s wake: with the lock held by another task, the resume
 * path waited 10 s here and another 10 s inside the elm327_set_baudrate() tail call, with the LED
 * dark and WiFi unreachable the whole time. The bounded form gives up quickly AND skips the tail
 * call when the lock was never obtained, so the worst case is the lock budget, not 20 s.
 *
 * elm327_hardreset_chip() is a wrapper passing ELM327_CMD_MUTEX_TIMOUT, so every pre-existing
 * caller keeps its old behaviour.
 *
 * To tell "could not get the lock" apart from "the chip is sick", read mutex_ok from
 * elm327_hardreset_get_timings() -- this call rewrites that record before it returns. */
bool elm327_hardreset_chip_timeout(uint32_t lock_wait_ms);

/* Name the task currently holding the UART lock, or "none"/"nosem". Does NOT take the lock, so it
 * is safe to call right after a failed acquisition -- which is its reason for existing. */
void elm327_lock_holder_name(char *dst, size_t dstlen);

/* Where the time goes inside elm327_hardreset_chip().
 *
 * Diagnostic only (issue: wake takes ~23 s instead of ~3.5 s). Measured live in the car on
 * 2026-08-17: every wake had the LED dark and WiFi down for ~23 s, and /wifi_diag put the first
 * WiFi association ATTEMPT 20 s after CAN_WAKE -- so ~20 s is spent in the resume BEFORE the
 * network is even touched, and elm327_hardreset_chip() is the only slow step there.
 *
 * Reading the code bounds it but cannot pin it, because one "hard reset" is really TWO mutex
 * acquisitions and up to six UART timeouts: this function takes xuart1_semaphore (up to
 * ELM327_CMD_MUTEX_TIMOUT = 10 s), waits 500 ms, reads once (~1.5 s), may fall through to the
 * reset line and read AGAIN (~1.5 s), releases the mutex -- and then calls elm327_set_baudrate(),
 * which takes the SAME mutex a second time (another 10 s worst case) and can do four more 1.2 s
 * reads. So these fields split the call into the pieces that can each be checked against their
 * own constant. */
typedef struct
{
	uint32_t mutex_ms;			/* waiting for xuart1_semaphore (cap ELM327_CMD_MUTEX_TIMOUT) */
	uint32_t reset_read_ms;		/* first uart_read_until_pattern (ATZ, or after the reset line) */
	uint32_t retry_read_ms;		/* the ATZ-failed fall-through read; 0 when not taken */
	uint32_t baudrate_ms;		/* elm327_set_baudrate() -- second mutex take + up to 4 reads */
	uint32_t total_ms;			/* the whole call, wall clock */
	uint32_t calls;				/* hard resets since boot -- catches "called more than once" */
	int8_t   gpio7_at_entry;	/* 1 = chip reports asleep, 0 = awake (LOW=awake; hw_config.h lies) */
	bool     used_reset_line;	/* false = ATZ path was believed sufficient */
	bool     mutex_ok;			/* false = the 10 s mutex wait TIMED OUT and nothing was done */
	bool     answered;			/* false = chip never sent the "\r>" prompt back */

	/* WHO HOLDS THE UART LOCK -- the one question the first round of instrumentation could not
	 * answer. Confirmed in the car 2026-08-17: a slow wake logged mutex=0(TIMEOUT) rd=0, i.e. the
	 * resume waited the full 10 s, never got the lock, never spoke to the chip at all, and then
	 * elm327_set_baudrate() burned a second 10 s the same way -- 20050 ms of the 20136 ms wake.
	 * So the chip was never slow; someone else was sitting on the lock. FreeRTOS can name the
	 * owning task, so these record it. "none" means the lock was free at that moment. */
	char     holder_before[16];	/* owner when elm327_hardreset_chip() arrived */
	char     holder_rst_to[16];	/* owner when the reset-path take TIMED OUT (else empty) */
	char     holder_baud_to[16];/* owner when elm327_set_baudrate()'s take TIMED OUT (else empty) */
} elm327_hardreset_timing_t;

/* Snapshot of the LAST elm327_hardreset_chip() call. Safe to call from another task. */
void elm327_hardreset_get_timings(elm327_hardreset_timing_t *out);

/* Undo elm327_sleep()'s GPIO9 pad hold. Required before the chip can be woken; called
 * from app_main at boot and from the sleep-resume path. */
void elm327_release_sleep_hold(void);
#endif
