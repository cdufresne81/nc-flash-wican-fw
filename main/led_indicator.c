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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led.h"
#include "led_indicator.h"
#include "can.h"
#include "csv_logger.h"
#include "config_server.h"
#include "dev_status.h"

#define TAG "LED_IND"

#define LED_IND_TICK_MS         250
// Hold red across the FLASH_ACTIVE_BIT gaps between the ~128 KB 'X' chunks of
// one logical fast-read, so a long ROM read shows steady red-blink, not flicker.
#define LED_IND_RED_HOLD_US     (1500LL * 1000LL)
#define LED_IND_RED_BRIGHTNESS  255
#define LED_IND_BLUE_BRIGHTNESS 200
// Red flash-activity software-blink half-period (ms) when blinking is enabled.
// Blue datalog uses the AW2023 hardware pattern instead (zero i2c); only this
// short-lived red blink is still software-timed on the RTOS tick.
#define LED_IND_FLASH_BLINK_MS  52

typedef enum {
    IND_UNKNOWN = 0,   // pre-first-paint; forces an initial repaint
    IND_IDLE,
    IND_FLASH_RED,
    IND_DATALOG_BLUE,
    IND_DEFERRED,      // a foreign owner (MIC update, sleep, config mode) holds the LED
} ind_state_t;

static SemaphoreHandle_t s_paint_mutex = NULL;
static volatile ind_state_t s_state = IND_UNKNOWN;
// Nested foreign-owner holds (MIC3624 update, sleep paths, config mode).
// Guarded by s_paint_mutex once it exists; boot-time calls before init are
// single-task.
static volatile int8_t s_suspend_count = 0;

static const char *ind_state_str(ind_state_t st)
{
    switch (st)
    {
        case IND_FLASH_RED:     return "flash_red";
        case IND_DATALOG_BLUE:  return "datalog_blue";
        case IND_DEFERRED:      return "deferred";
        case IND_IDLE:
        default:                return "idle";
    }
}

const char *led_indicator_get_state_str(void)
{
    return ind_state_str(s_state);
}

void led_indicator_suspend(void)
{
    if (s_paint_mutex == NULL)
    {
        // Pre-init (boot-time MIC update): no task is painting yet, and the LED
        // i2c may not be up -- do not touch it here.
        s_suspend_count++;
        return;
    }
    // Blocks until any in-progress repaint finished; after this returns the
    // indicator task will not touch the LED until led_indicator_resume().
    xSemaphoreTake(s_paint_mutex, portMAX_DELAY);
    if (s_suspend_count == 0)
    {
        // Outermost suspend: hand a clean LED to the foreign owner. If we were in
        // DATALOG_BLUE the AW2023 is still blinking blue autonomously (MD bit),
        // and the task will NOT clear it because it stops painting in DEFERRED.
        // Tear down both channels' patterns now -- one-time ~4 tx at handoff, off
        // the sustained SD path. Restores the invariant that held for free when
        // DATALOG_BLUE was a solid/software state.
        led_disable_pattern(LED_RED);
        led_disable_pattern(LED_BLUE);
    }
    s_suspend_count++;
    xSemaphoreGive(s_paint_mutex);
}

void led_indicator_resume(void)
{
    if (s_paint_mutex == NULL)
    {
        if (s_suspend_count > 0) s_suspend_count--;
        return;
    }
    xSemaphoreTake(s_paint_mutex, portMAX_DELAY);
    if (s_suspend_count > 0) s_suspend_count--;
    // Force a repaint on the next tick so the idle color comes back even if
    // the foreign owner left the LED dark (pre-existing MIC-update behavior).
    s_state = IND_UNKNOWN;
    xSemaphoreGive(s_paint_mutex);
}

// Paint one phase of the software blink: the state's channel at its brightness
// or all channels dark. Blink amplitude and timing both live here — the AW2023
// runs in plain PWM mode, no hardware pattern.
static void ind_paint(ind_state_t state, bool phase_on)
{
    switch (state)
    {
        case IND_FLASH_RED:
            led_set_level(phase_on ? LED_IND_RED_BRIGHTNESS : 0, 0, 0);
            break;
        case IND_DATALOG_BLUE:
            led_set_level(0, 0, phase_on ? LED_IND_BLUE_BRIGHTNESS : 0);
            break;
        case IND_IDLE:
        default:
            led_set_level(LED_IND_IDLE_R, LED_IND_IDLE_G, LED_IND_IDLE_B);
            break;
    }
}

static void led_indicator_task(void *pvParameters)
{
    int64_t red_hold_until_us = 0;
    bool phase_on = false;
    // Tracks whether the last paint used the blink form, so a live led_blink
    // toggle is honored even when the indicator state itself doesn't change.
    bool blink_applied = false;

    for (;;)
    {
        // Painting is gated on the device being awake, same as config_mode_task;
        // the sleep paths additionally suspend() before writing the LED off.
        dev_status_wait_for_bits(DEV_AWAKE_BIT, portMAX_DELAY);

        // Live-apply toggle: enable = blink while active, disable = solid color.
        const bool blink_on = (config_server_get_led_blink_enabled() != 0);

        // Decide and paint under the mutex: once led_indicator_suspend()
        // returns, this task must not touch the LED. The predicates are all
        // lock-free flag reads, so holding the mutex across them is cheap.
        xSemaphoreTake(s_paint_mutex, portMAX_DELAY);
        ind_state_t desired;
        if (s_suspend_count > 0)
        {
            desired = IND_DEFERRED;
        }
        else if (can_flash_active())
        {
            desired = IND_FLASH_RED;
            red_hold_until_us = esp_timer_get_time() + LED_IND_RED_HOLD_US;
        }
        else if (esp_timer_get_time() < red_hold_until_us)
        {
            desired = IND_FLASH_RED;   // chunk-gap hysteresis
        }
        else if (csv_logger_session_active() && !can_should_park())
        {
            desired = IND_DATALOG_BLUE;
        }
        else
        {
            desired = IND_IDLE;
        }

        // dev_status_is_awake() is a backstop for any sleep path without a
        // suspend() hook — never paint over a LED the sleep code turned off.
        if (desired != IND_DEFERRED && dev_status_is_awake())
        {
            // Repaint on a state change OR a live blink-toggle change.
            if (desired != s_state || blink_on != blink_applied)
            {
                // The MD (pattern-mode) bit survives led_set_level: clear both
                // channels' patterns when (re)taking the LED so a leftover
                // hardware pattern can't fight a software blink or a solid state.
                // This also guarantees MD makes a real 0->1 edge below.
                led_disable_pattern(LED_RED);
                led_disable_pattern(LED_BLUE);
                phase_on = true;   // enter software-blink states visibly on
                if (desired == IND_DATALOG_BLUE && blink_on)
                {
                    // interrupt_wdt fix: hand the "logging active" blink to the
                    // AW2023 pattern engine. Programmed ONCE here; the chip then
                    // blinks blue (~3.85 Hz) on its own with ZERO i2c for the
                    // whole sustained state, so nothing co-tenants the core-0
                    // SD-write path. (A software toggle here drove ~115 i2c tx/s
                    // and starved the tick past INT_WDT; 0 tx/s is the only
                    // bench-proven-safe level.)
                    led_datalog_blink_hw(LED_IND_BLUE_BRIGHTNESS);
                }
                else
                {
                    // Solid: idle, blink-disabled datalog blue, or the "on" phase
                    // of the red flash blink. One-shot write, then no further i2c.
                    ind_paint(desired, phase_on);
                }
                blink_applied = blink_on;
            }
            // FLASH_RED keeps its software blink while enabled -- short-lived and
            // never overlaps the sustained SD-write path the WDT amplifier needs.
            // DATALOG_BLUE (== s_state, blink unchanged) deliberately falls
            // through with NO i2c -- its hardware pattern / solid paint still holds.
            else if (desired == IND_FLASH_RED && blink_on)
            {
                phase_on = !phase_on;
                ind_paint(desired, phase_on);
            }
        }
        const ind_state_t prev = s_state;
        s_state = desired;
        xSemaphoreGive(s_paint_mutex);

        if (desired != prev)
        {
            ESP_LOGI(TAG, "state %s -> %s (blink %s)",
                     ind_state_str(prev), ind_state_str(desired),
                     blink_on ? "on" : "off");
        }
        // Only the red software blink needs the fast tick; every other state is
        // painted once and idles on the slow tick -- no per-tick i2c during
        // sustained logging.
        const bool fast_tick = (desired == IND_FLASH_RED && blink_on);
        vTaskDelay(pdMS_TO_TICKS(fast_tick ? LED_IND_FLASH_BLINK_MS : LED_IND_TICK_MS));
    }
}

void led_indicator_init(void)
{
    if (s_paint_mutex != NULL)
    {
        return;
    }
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create paint mutex");
        return;
    }
    // Publish the mutex before the task exists (csv_logger boot-race lesson:
    // publish state before creating the task that reads it).
    s_paint_mutex = mutex;
    if (s_suspend_count > 0)
    {
        ESP_LOGI(TAG, "starting with %d pre-init suspend hold(s)", s_suspend_count);
    }
    if (xTaskCreateWithCaps(led_indicator_task, "led_ind_task", 3072, NULL, 2, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create led_ind_task");
        vSemaphoreDelete(mutex);
        s_paint_mutex = NULL;
    }
}
