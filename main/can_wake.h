/*
 * Wake-on-CAN (issue #4) -- wake the device from light sleep when the car starts talking.
 *
 * This is only possible because of what issue #89 proved on real hardware: the TJA1044
 * transceiver's RXD pin keeps following the bus while the part sits in Standby at ~0.01 mA.
 * Datasheet Table 4: in Standby, RXD "follows BUS when wake-up detected, HIGH when no wake-up
 * detected". Measured on this board -- 5714 edges per 300 ms on a busy bus, exactly 0 on a quiet
 * one, and a key turn in a real car caught inside a single 20 ms sample.
 *
 * RXD is RX_GPIO_NUM (GPIO1). While asleep the TWAI driver is uninstalled, so the pin is a plain
 * input and the ESP32-S3 can wake from LIGHT sleep on its level (esp_sleep.h: ESP_SLEEP_WAKEUP_GPIO
 * is light-sleep only on ESP32/S2/S3). That costs nothing while idle, unlike polling.
 *
 * The transceiver gives us a free hardware debounce: it only releases RXD after its own wake-up
 * pattern (dominant/recessive/dominant, each phase 0.5-3 us) has been matched, so sub-microsecond
 * noise cannot even move the pin.
 */

#ifndef __CAN_WAKE_H__
#define __CAN_WAKE_H__

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CAN_WAKE_QUIET = 0,   /* nothing convincing happened -- go back to sleep */
    CAN_WAKE_CONFIRMED,   /* real, sustained bus traffic -- the car is awake */
    CAN_WAKE_STUCK_LOW,   /* RXD held LOW and never returned HIGH -- a fault, NOT activity */
} can_wake_verdict_t;

/* Is the feature switched on? Reads the `can_wake` config key; enabled unless explicitly
 * "disable", so a device provisioned before this key existed gets the feature. */
bool can_wake_enabled(void);

/* Put GPIO1 into the state the sampler needs: plain input, BOTH internal pulls OFF.
 * Call once per sleep entry, AFTER can_disable() has uninstalled TWAI and freed the pin.
 * Deliberately never calls gpio_reset_pin(): that switches the internal pull-up ON, and because
 * RXD is push-pull a pulled-up dead RXD reads HIGH -- indistinguishable from a quiet bus.
 * SAFETY: GPIO1 is the transceiver's push-pull OUTPUT and must NEVER be driven as an output. */
void can_wake_prepare(void);

typedef enum {
    CAN_WAKE_ARM_OK = 0,       /* armed -- a GPIO wake can now fire during light sleep */
    CAN_WAKE_ARM_SKIP_LOW,     /* pin was ALREADY LOW -- the caller must confirm NOW, not after
                                * sleeping: the wake condition may already be true, and arming a
                                * level trigger on an asserted level wakes instantly, forever. */
    CAN_WAKE_ARM_SKIP_OTHER,   /* disabled, stuck-RXD fault, or fruitless cooldown -- do nothing */
} can_wake_arm_t;

/* Arm the level-LOW GPIO wake for the next esp_light_sleep_start(), if it is safe to do so.
 * The return value tells the caller WHY it did not arm, which matters: SKIP_LOW needs an
 * immediate confirm, SKIP_OTHER needs silence. */
can_wake_arm_t can_wake_arm(void);

/* Disarm the GPIO wake source. MUST be the first thing called after esp_light_sleep_start()
 * returns, before any branch: wake-source flags persist across sleep calls, so a trigger left
 * armed on a LOW pin livelocks the sleep loop. */
void can_wake_disarm(void);

/* Sample RXD and decide whether the bus is genuinely alive. Runs to at most
 * CAN_WAKE_CONFIRM_MS, exiting early the moment the thresholds are met (usually a few ms).
 * The out-params are for the event log; any may be NULL. */
can_wake_verdict_t can_wake_confirm(uint32_t *edges, uint32_t *rises, uint32_t *elapsed_ms);

/* Feed a verdict back so the module can maintain its fault and cooldown state. Returns true if
 * the caller should now reboot into normal operation. */
bool can_wake_handle(can_wake_verdict_t v, uint32_t edges, uint32_t rises, uint32_t elapsed_ms);

/* --- Fruitless-wake accounting -------------------------------------------------------------
 * A wake that never leads to the engine actually running has cost battery for nothing. Three of
 * those in a row and the feature throttles itself for an hour. Because a wake now resumes in
 * place instead of rebooting, this is scoped to a WAKE WINDOW, not to a boot:
 *
 *     can_wake_note_wake()         opens a window   (every wake that is acted on)
 *     can_wake_note_voltage_ok()   marks it worthwhile (engine started)
 *     can_wake_note_sleep_entry()  closes and scores it (every sleep entry)
 *
 * Scoring at sleep entry is what makes it correct: that is the one point every wake passes
 * through exactly once, whether it ended in a resume, the reboot fallback, or a periodic reboot.
 */

/* Called once at boot, before the sleep loop starts. Restores the streak from its RTC mirror and,
 * when `woke_on_can` says this boot WAS a CAN wake, opens a window for it so a wake that came
 * back the reboot way is scored identically to one that resumed. */
void can_wake_boot_init(bool woke_on_can);

/* Open a wake window. Call on every CAN wake that is acted on -- both the resume path and just
 * before a fallback reboot. */
void can_wake_note_wake(void);

/* Close and score the open window, if any. Call from the sleep teardown, on every sleep entry. */
void can_wake_note_sleep_entry(void);

/* Called when the voltage is seen to recover, i.e. the engine really did start. Marks the open
 * window as worthwhile; the streak is cleared when that window closes. */
void can_wake_note_voltage_ok(void);

#endif /* __CAN_WAKE_H__ */
