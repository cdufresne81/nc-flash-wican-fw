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
} elm327_hardreset_timing_t;

/* Snapshot of the LAST elm327_hardreset_chip() call. Safe to call from another task. */
void elm327_hardreset_get_timings(elm327_hardreset_timing_t *out);

/* Undo elm327_sleep()'s GPIO9 pad hold. Required before the chip can be woken; called
 * from app_main at boot and from the sleep-resume path. */
void elm327_release_sleep_hold(void);
#endif
