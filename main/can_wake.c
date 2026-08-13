/*
 * Wake-on-CAN (issue #4). See can_wake.h for the hardware basis and the #89 measurements.
 */

#include <string.h>
#include <time.h>   /* wall-clock deadline mirrored to RTC, for carrying a cooldown over a reboot */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "hw_config.h"
#include "can_wake.h"
#include "config_server.h"
#include "event_log.h"

static const char *TAG = "can_wake";

/* --- Tuning. Hard-coded on purpose: this is a single-vehicle product, and every extra config
 * key is another way the stored-config parser can reject a device into a factory reset (#44).
 * Every number below is derived from the #89 bench and car measurements. --------------------- */

/* Ceiling for the confirmation sample. >10x the worst inter-frame gap we measured (9.4 ms), so a
 * single quiet gap can never be mistaken for a dead bus. Almost always exits far earlier. */
#define CAN_WAKE_CONFIRM_MS        100

/* A live bus gave ~1900 edges per 100 ms; one single CAN frame gives 30+. 20 therefore sits far
 * under real traffic and far over any glitch that survives the transceiver's own wake filter. */
#define CAN_WAKE_MIN_EDGES         20

/* At least two returns to recessive. This is what makes a stuck-dominant bus structurally unable
 * to confirm: a line held LOW produces no rising edges at all. */
#define CAN_WAKE_MIN_RISES         2

/* Consecutive stuck readings before we stop arming, and consecutive clean reads to recover. */
#define CAN_WAKE_STUCK_STRIKES     3
#define CAN_WAKE_RECOVER_READS     2

/* Confirmed-but-pointless wakes (woke on CAN, engine never started) before we throttle. Bounds
 * the cost if something on the car chirps the bus while parked. */
#define CAN_WAKE_FRUITLESS_LIMIT   3
#define CAN_WAKE_COOLDOWN_S        3600

/* The live counter is plain RAM (see below); this RTC copy exists only so the streak survives a
 * reboot -- the fallback path, a crash, a periodic restart. Lost on power removal, which is the
 * safe direction: unplugging the dongle always restores a fully-armed feature.
 *
 * BUMP THIS MAGIC WHENEVER can_wake_rtc_t CHANGES SHAPE. RTC memory is not cleared by a firmware
 * update, so a device upgrading across a layout change would keep the old magic, pass the
 * validity check, and read whatever bytes happen to sit at the new field's offset. Bumping forces
 * a clean reset on the first boot of the new firmware, which costs at most a few extra wakes --
 * the permissive direction. ('CWK3' = added cooldown_until_epoch.) */
#define CAN_WAKE_RTC_MAGIC         0x43574B33u   /* 'CWK3' */

typedef struct {
    uint32_t magic;
    uint32_t fruitless;            /* MIRROR of s_fruitless, so a reboot does not lose the streak */
    int64_t  cooldown_until_epoch; /* wall-clock second the cooldown ends; 0 = none armed */
} can_wake_rtc_t;

static RTC_NOINIT_ATTR can_wake_rtc_t s_rtc;

static bool     s_armed          = false;
static uint8_t  s_stuck_strikes  = 0;
static uint8_t  s_clean_reads    = 0;
static bool     s_rxd_fault      = false;
static bool     s_fault_logged   = false;
static bool     s_cooldown_logged = false;  /* per cooldown EPISODE, not per boot -- see below */

/* --- Fruitless-wake accounting, scoped to a WAKE WINDOW rather than to a boot ---------------
 *
 * A "window" opens when a CAN wake is acted on and closes when the device goes back to sleep.
 * Scoring at sleep entry (rather than at the next boot) is what makes this work now that a wake
 * resumes in place: the close is guaranteed to happen exactly once per wake, whether that wake
 * ended in a resume, a fallback reboot, or a periodic reboot. The old version keyed everything
 * to "one boot == one wake", so with no reboot it never scored anything at all and the battery
 * protection was silently dead.
 *
 * The LIVE cooldown runs on esp_timer (monotonic microseconds since boot). It keeps counting
 * across light sleep, needs no NTP, and cannot jump backwards -- so a device that never gets the
 * time still gets its cooldown. esp_timer restarts at zero on every boot, so a wall-clock
 * DEADLINE is mirrored to RTC purely to carry the remaining time across a reboot; see
 * can_wake_boot_init(). Power removal wipes RTC memory, which is deliberate: unplugging the
 * dongle always restores a fully-armed feature.
 *
 * LOAD-BEARING PROPERTY: a SUPPRESSED wake must never open a window. can_wake_handle() refuses
 * before anything calls can_wake_note_wake(), so a chattering bus cannot keep pushing the
 * deadline outward and lock the feature off forever. The hour always genuinely lapses. */
static bool     s_wake_open        = false; /* a CAN-wake window is open right now */
static bool     s_volt_ok_seen     = false; /* voltage recovered since THIS window opened */
/* ECU answered since THIS window opened. Plain RAM on purpose: it is window-scoped by definition,
 * so it must NOT go in can_wake_rtc_t -- which also means this addition needs no magic bump. */
static bool     s_ecu_ok_seen      = false;
static uint32_t s_fruitless        = 0;     /* consecutive wakes that never saw the voltage rise */
static int64_t  s_cooldown_until_us = 0;    /* esp_timer deadline; 0 = no cooldown armed */

bool can_wake_enabled(void)
{
    return (config_server_get_can_wake() != 0);
}

void can_wake_boot_init(bool woke_on_can)
{
    if (s_rtc.magic != CAN_WAKE_RTC_MAGIC)
    {
        /* First boot after power-on, or corrupted RTC RAM after a brownout. Reset to the
         * permissive state: worst case is a few extra wakes, never a feature stuck off. */
        s_rtc.magic                = CAN_WAKE_RTC_MAGIC;
        s_rtc.fruitless            = 0;
        s_rtc.cooldown_until_epoch = 0;
    }
    else if (s_rtc.fruitless > 100)
    {
        /* Magic intact but the value is absurd -- a torn write across a brownout. Reset rather
         * than let a garbage count lodge the feature in permanent cooldown. */
        s_rtc.fruitless            = 0;
        s_rtc.cooldown_until_epoch = 0;
    }

    /* Restore the streak the mirror was holding across the reboot. */
    s_fruitless         = s_rtc.fruitless;
    s_wake_open         = false;
    s_volt_ok_seen      = false;
    s_cooldown_until_us = 0;

    if (woke_on_can)
    {
        /* This boot IS a wake, so open a window for it right now. That makes a wake that came
         * back through the reboot fallback score exactly like one that resumed in place -- at
         * the next sleep entry, through the same code. */
        can_wake_note_wake();
        ESP_LOGI(TAG, "boot caused by CAN wake (fruitless streak %u)", (unsigned)s_fruitless);
    }
    else if (s_fruitless >= CAN_WAKE_FRUITLESS_LIMIT)
    {
        /* Rebooted while a cooldown was in force. The streak survived in RTC memory but the
         * esp_timer deadline did not -- esp_timer restarts at zero on every boot and no monotonic
         * clock outlives a reset. So carry the REMAINING time across on the wall clock.
         *
         * This works even with no NTP: the RTC-backed clock is CONSISTENT across a soft reboot
         * and only differences are taken, so an unsynced 1970 epoch still measures an hour
         * correctly. A forward SNTP jump mid-cooldown ends it early -- the permissive direction,
         * costing at most one extra wake.
         *
         * The rejected alternative was "restart the hour on every boot". It ties throttling to
         * reboot frequency, which is exactly the assumption this codebase cannot afford, and it
         * would let anything that reboots often stretch a cooldown indefinitely. */
        const int64_t now   = (int64_t)time(NULL);
        const int64_t until = s_rtc.cooldown_until_epoch;

        if (until > now && (until - now) <= (2 * (int64_t)CAN_WAKE_COOLDOWN_S))
        {
            s_cooldown_until_us = esp_timer_get_time() + ((until - now) * 1000000LL);
            ESP_LOGW(TAG, "boot during cooldown -- %lld s of throttling left (streak %u)",
                     (long long)(until - now), (unsigned)s_fruitless);
        }
        else if (until > 0 && until <= now)
        {
            /* The hour already served its time while we were away. Leave it disarmed: the next
             * wake is allowed, and if it is fruitless too the streak is already at the limit so
             * a fresh cooldown arms immediately at the following sleep entry. */
            s_rtc.cooldown_until_epoch = 0;
            ESP_LOGI(TAG, "boot with streak %u but the cooldown has expired -- arming normally",
                     (unsigned)s_fruitless);
        }
        else
        {
            /* No deadline stored, garbage, or the clock ran backwards. Degraded path: serve a
             * full hour from here rather than hand out a free wake. */
            s_cooldown_until_us        = esp_timer_get_time() + ((int64_t)CAN_WAKE_COOLDOWN_S * 1000000LL);
            s_rtc.cooldown_until_epoch = now + CAN_WAKE_COOLDOWN_S;
            ESP_LOGW(TAG, "boot with streak %u and no usable deadline -- serving a full %us",
                     (unsigned)s_fruitless, (unsigned)CAN_WAKE_COOLDOWN_S);
        }
    }
}

void can_wake_note_wake(void)
{
    /* Only ever reached for a wake that was ALLOWED. A wake suppressed by the cooldown returns
     * from can_wake_handle() before this, which is what guarantees the deadline below is never
     * pushed outward by the very traffic it is throttling. */
    s_wake_open    = true;
    s_volt_ok_seen = false;
    s_ecu_ok_seen  = false;
}

void can_wake_note_sleep_entry(void)
{
    if (!s_wake_open) return;   /* this sleep did not follow a CAN wake -- nothing to score */
    s_wake_open = false;

    /* EITHER proof counts (issue #4). Voltage alone was wrong and cost us a real car test: the
     * dongle woke, the ECU answered for a full minute and wrote 500+ KB of genuine engine data,
     * and because the supply never crossed sleep_volt+0.1 all three of those wakes scored
     * fruitless and switched wake-on-CAN off for an hour. An answering ECU proves the wake led
     * somewhere just as well as a charging alternator does.
     *
     * The voltage half STAYS as the mode-independent fallback. poll_log_ecu_answering() is
     * permanently false in ELM327/FAST_LOG mode, so scoring on the ECU alone would call every
     * wake in those modes fruitless and throttle the feature on a perfectly working car after
     * three ordinary mornings. */
    if (s_volt_ok_seen || s_ecu_ok_seen)
    {
        if (s_fruitless != 0)
        {
            /* "led somewhere", not "led to a running engine": the ECU answers at key-on with the
             * engine still off, and that counts. Only the voltage proof implies a real start. */
            ESP_LOGI(TAG, "wake led somewhere (%s) -- fruitless streak cleared (was %u)",
                     s_ecu_ok_seen ? "ECU answering" : "voltage recovered", (unsigned)s_fruitless);
        }
        s_fruitless                = 0;
        s_cooldown_until_us        = 0;
        s_rtc.cooldown_until_epoch = 0;
    }
    else
    {
        s_fruitless++;
        ESP_LOGW(TAG, "wake was fruitless (no ECU reply, voltage never recovered) -- streak now %u/%u",
                 (unsigned)s_fruitless, (unsigned)CAN_WAKE_FRUITLESS_LIMIT);
        if (s_fruitless >= CAN_WAKE_FRUITLESS_LIMIT)
        {
            /* Arm (or re-arm) the throttle. Both axes: esp_timer for the live check, and the
             * wall-clock mirror so a reboot can carry the remainder instead of losing it. */
            s_cooldown_until_us        = esp_timer_get_time() + ((int64_t)CAN_WAKE_COOLDOWN_S * 1000000LL);
            s_rtc.cooldown_until_epoch = (int64_t)time(NULL) + CAN_WAKE_COOLDOWN_S;

            if (s_fruitless == CAN_WAKE_FRUITLESS_LIMIT)
            {
                event_log_emit(EVL_CAN_WAKE, "%u fruitless wakes -- throttling wake-on-CAN for %us",
                               (unsigned)s_fruitless, (unsigned)CAN_WAKE_COOLDOWN_S);
            }
        }
    }

    s_rtc.fruitless = s_fruitless;   /* mirror, so the reboot fallback does not lose the streak */
}

void can_wake_note_voltage_ok(void)
{
    /* CLEARING is deliberately NOT gated on an open window, while SCORING still is.
     *
     * The cooldown exists to bound one hypothesis: "something chirps the bus but the car is not
     * actually being driven". An engine that demonstrably runs refutes that hypothesis outright,
     * and it does not matter one bit which path woke the device to see it. Gating the clear on a
     * CAN-wake window created a trap: a tripped cooldown suppresses CAN wakes, so mornings then
     * start via the VOLTAGE path, which never opens a window -- and the streak could not clear
     * itself through the very event that proves it wrong.
     *
     * Cost of the loosened rule: a car that both chirps while parked AND gets driven daily has
     * its 3-wake budget restored each day. That is the same budget the design grants any normal
     * car, so it is correct behaviour rather than a leak.
     *
     * This runs on the ~500 ms voltage sampling path, so it does nothing at all in the common
     * case where there is no streak to clear. */
    if (s_fruitless != 0)
    {
        ESP_LOGI(TAG, "engine running -- fruitless streak cleared (was %u)", (unsigned)s_fruitless);
        event_log_emit(EVL_CAN_WAKE, "engine running -- wake-on-CAN throttle cleared (streak was %u)",
                       (unsigned)s_fruitless);
        s_fruitless                = 0;
        s_rtc.fruitless            = 0;
        s_cooldown_until_us        = 0;
        s_rtc.cooldown_until_epoch = 0;
    }

    /* Window scoring is unchanged: this only credits the wake that is actually open. */
    if (s_wake_open) s_volt_ok_seen = true;
}

/* The ECU is answering our polls, i.e. the ignition is on. Deliberately bit-for-bit parallel to
 * can_wake_note_voltage_ok() above -- including the un-windowed clear and both RTC mirror writes --
 * so the two clearing paths can never drift apart and leave the cooldown half-reset. Read the long
 * comment above for why CLEARING is not gated on an open window while SCORING is.
 *
 * Called from the same ~500 ms sampling path as the voltage version, so it costs nothing in the
 * common case where there is no streak to clear. */
void can_wake_note_ecu_ok(void)
{
    if (s_fruitless != 0)
    {
        ESP_LOGI(TAG, "ECU answering -- fruitless streak cleared (was %u)", (unsigned)s_fruitless);
        event_log_emit(EVL_CAN_WAKE, "ECU answering -- wake-on-CAN throttle cleared (streak was %u)",
                       (unsigned)s_fruitless);
        s_fruitless                = 0;
        s_rtc.fruitless            = 0;
        s_cooldown_until_us        = 0;
        s_rtc.cooldown_until_epoch = 0;
    }

    /* Known leak, bounded and deliberately accepted: the ONLY way to enter sleep with the ECU
     * signal still true is the forced boot-loop teardown, and on the next wake the poller needs a
     * few seconds to notice the silence and clear s_ecu_answering. That window could wrongly credit
     * one wake. It errs toward keeping the feature armed, which is the safe direction. */
    if (s_wake_open) s_ecu_ok_seen = true;
}

/* True while we are throttling because recent CAN wakes never led to an engine start. */
static bool can_wake_in_cooldown(void)
{
    if (s_cooldown_until_us <= 0) return false;   /* nothing armed */
    if (esp_timer_get_time() >= s_cooldown_until_us)
    {
        s_cooldown_until_us        = 0;           /* served its time */
        s_rtc.cooldown_until_epoch = 0;
        return false;
    }
    return true;
}

void can_wake_prepare(void)
{
    /* NOT gpio_reset_pin() -- see the header. TWAI's uninstall (IDF twai.c:338-354) leaves the RX
     * pad alone anyway, so this only makes the required state explicit. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << RX_GPIO_NUM,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

can_wake_arm_t can_wake_arm(void)
{
    s_armed = false;

    if (!can_wake_enabled()) return CAN_WAKE_ARM_SKIP_OTHER;

    if (s_rxd_fault)
    {
        /* Fault recovery is a cheap level read once per timer wake; no sampling, no arming. */
        if (gpio_get_level(RX_GPIO_NUM) == 1)
        {
            if (++s_clean_reads >= CAN_WAKE_RECOVER_READS)
            {
                s_rxd_fault     = false;
                s_fault_logged  = false;
                s_stuck_strikes = 0;
                s_clean_reads   = 0;
                ESP_LOGW(TAG, "RXD recovered -- arming resumed");
                event_log_emit(EVL_INFO, "can_wake: RXD recovered, arming resumed");
            }
        }
        else
        {
            s_clean_reads = 0;
        }
        return CAN_WAKE_ARM_SKIP_OTHER;
    }

    if (can_wake_in_cooldown()) return CAN_WAKE_ARM_SKIP_OTHER;

    /* The anti-livelock rule. A level trigger on an already-asserted level fires the instant we
     * sleep, forever. LOW here means traffic is flowing right now, or the bus is stuck dominant;
     * either way the caller must decide NOW rather than sleep on it. */
    if (gpio_get_level(RX_GPIO_NUM) == 0) return CAN_WAKE_ARM_SKIP_LOW;

    if (gpio_wakeup_enable(RX_GPIO_NUM, GPIO_INTR_LOW_LEVEL) != ESP_OK) return CAN_WAKE_ARM_SKIP_OTHER;
    if (esp_sleep_enable_gpio_wakeup() != ESP_OK)
    {
        gpio_wakeup_disable(RX_GPIO_NUM);
        return CAN_WAKE_ARM_SKIP_OTHER;
    }

    s_armed = true;
    return CAN_WAKE_ARM_OK;
}

void can_wake_disarm(void)
{
    if (!s_armed) return;
    s_armed = false;
    gpio_wakeup_disable(RX_GPIO_NUM);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
}

/* Tight sampler, in IRAM so a flash-cache stall cannot silently slow it. Interrupts stay enabled:
 * we are counting "zero versus many", not decoding a bitstream. The #89 spike measured this same
 * loop at ~3.57 Msps, which is ~7 samples per 2 us bit at 500 kbit/s. */
can_wake_verdict_t IRAM_ATTR can_wake_confirm(uint32_t *edges_out, uint32_t *rises_out,
                                              uint32_t *elapsed_ms)
{
    const int64_t t0   = esp_timer_get_time();
    const int64_t tend = t0 + (int64_t)CAN_WAKE_CONFIRM_MS * 1000;

    int      prev  = gpio_get_level(RX_GPIO_NUM);
    bool     low   = (prev == 0);
    uint32_t edges = 0, rises = 0, tick = 0;
    int64_t  now   = t0;

    for (;;)
    {
        const int v = gpio_get_level(RX_GPIO_NUM);
        if (v != prev)
        {
            edges++;
            if (v == 0) low = true;
            else        rises++;
            prev = v;

            if (edges >= CAN_WAKE_MIN_EDGES && rises >= CAN_WAKE_MIN_RISES)
            {
                now = esp_timer_get_time();
                break;               /* early exit -- decided */
            }
        }

        if ((++tick & 0x3F) == 0)
        {
            now = esp_timer_get_time();
            if (now >= tend) break;
        }
    }

    if (edges_out)  *edges_out  = edges;
    if (rises_out)  *rises_out  = rises;
    if (elapsed_ms) *elapsed_ms = (uint32_t)((now - t0) / 1000);

    if (edges >= CAN_WAKE_MIN_EDGES && rises >= CAN_WAKE_MIN_RISES) return CAN_WAKE_CONFIRMED;

    /* Went LOW (or started LOW) and never came back up: a held-dominant line, not traffic.
     * The TJA1044's own bus-dominant timeout was DELETED in datasheet Rev. 6, so this software
     * path is the only recovery there is. */
    if (rises == 0 && low) return CAN_WAKE_STUCK_LOW;

    return CAN_WAKE_QUIET;
}

bool can_wake_handle(can_wake_verdict_t v, uint32_t edges, uint32_t rises, uint32_t elapsed_ms)
{
    switch (v)
    {
        case CAN_WAKE_CONFIRMED:
            s_stuck_strikes = 0;

            /* The cooldown must be enforced HERE, not only at arm time. Suppressing arming alone
             * is not a bound: a continuously chattering bus -- precisely the case the cooldown
             * exists for -- can still reach this path, and without this check it would confirm
             * and wake every few seconds forever. This is the single guard that keeps a
             * misbehaving car from flattening the battery, so it gates the wake itself. */
            if (can_wake_in_cooldown())
            {
                /* One line per cooldown EPISODE, not one per boot. The device no longer reboots
                 * to wake, so a plain once-per-boot latch would report the first episode and then
                 * stay silent through every later one for the rest of an uptime measured in
                 * weeks -- the same disease as the boot-scoped self-heals. s_cooldown_logged is
                 * reset below, when a wake is actually allowed through. */
                if (!s_cooldown_logged)
                {
                    s_cooldown_logged = true;
                    ESP_LOGW(TAG, "bus alive but in cooldown (%u fruitless) -- staying asleep",
                             (unsigned)s_fruitless);
                    event_log_emit(EVL_CAN_WAKE,
                                   "confirmed but cooling down (%u fruitless) -- staying asleep",
                                   (unsigned)s_fruitless);
                }
                return false;
            }

            s_cooldown_logged = false;   /* a wake got through: re-arm the log for the next episode */
            ESP_LOGW(TAG, "bus alive: %u edges (%u rise) in %u ms -> wake",
                     (unsigned)edges, (unsigned)rises, (unsigned)elapsed_ms);
            /* #98: the WAKE is news and always gets a line; the edge counts are diagnostics and do
             * not. One line either way -- the detailed form already says everything the plain form
             * does, and the RAM ring is only 64 lines deep, so a debugging session that wakes the
             * device repeatedly should not spend two slots per wake on one fact. */
            if (event_log_debug_enabled())
            {
                event_log_emit(EVL_CAN_WAKE, "confirmed: %u edges (%u rise) in %u ms -> wake",
                               (unsigned)edges, (unsigned)rises, (unsigned)elapsed_ms);
            }
            else
            {
                event_log_emit(EVL_CAN_WAKE, "woken by CAN bus activity");
            }
            return true;

        case CAN_WAKE_STUCK_LOW:
            /* Saturate rather than wrap: the fault latches at the threshold and this keeps
             * counting, so a uint8_t would roll over at 255 and briefly un-trip a future
             * threshold check. */
            if (s_stuck_strikes < 255) s_stuck_strikes++;
            if (s_stuck_strikes >= CAN_WAKE_STUCK_STRIKES && !s_rxd_fault)
            {
                s_rxd_fault    = true;
                s_clean_reads  = 0;
                if (!s_fault_logged)
                {
                    s_fault_logged = true;
                    ESP_LOGE(TAG, "RXD stuck LOW -- arming suspended (bus held dominant?)");
                    event_log_emit(EVL_CAN_WAKE, "RXD stuck LOW -> arming suspended");
                }
            }
            return false;

        case CAN_WAKE_QUIET:
        default:
            s_stuck_strikes = 0;
            return false;
    }
}

/* can_wake_count_wake() is gone. It scored the PREVIOUS wake at the moment the next one started,
 * which only worked while every wake rebooted. Scoring now happens at sleep entry --
 * can_wake_note_sleep_entry() -- where each wake is closed exactly once regardless of whether it
 * ended in a resume or a reboot. */
