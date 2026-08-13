/* Host tests for the CSV datalogger's bring-up and gate decisions.
 *
 * These exist because of a field failure that no test could have caught: on v1.18 a
 * customer's datalogger stopped auto-starting at engine-on, manual Start still worked,
 * rebooting did not help, and only unplugging the dongle from the OBD port fixed it.
 *
 * The cause was two latches, reproduced below as T1 and T3:
 *
 *   T1  The RTC crash guard is armed for the first ~15 s of every uptime. A reboot
 *       inside that window makes the NEXT boot skip CSV bring-up entirely -- and before
 *       the fix, that skip cost the WHOLE uptime with no retry. RTC memory survives a
 *       software reboot but not a power cut, which is exactly why rebooting did not help
 *       and unplugging did.
 *
 *   T3  The web Stop button latched force-off until a reboot, so a later ignition-on
 *       recorded nothing.
 *
 * T2 and T4 are the invariants the fix must not break: the boot-loop bound the guard
 * exists to provide, and the logging gate's truth table (which the investigation cleared
 * -- any change there would be a regression, not a fix).
 *
 * Build + run: tools/hosttest/run.sh
 */

#include <stdio.h>
#include <string.h>

#include "csv_bringup_logic.h"

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("  not ok %d - ", g_checks);                                 \
            printf(__VA_ARGS__);                                                \
            printf("\n         at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)

static void banner(const char *name)
{
    printf("# %s\n", name);
}

/* ---------------------------------------------------------------------------
 * T1 -- the reported failure: a reboot inside the guard window costs auto-logging.
 *
 * Replays the customer's event log directly:
 *   12:39:58  BOOT                     -> guard armed
 *   12:40:20  DATALOG_OPEN up=22762ms  -> auto-start worked
 *   12:40:23  BOOT user_request        -> up=25s, still inside the armed window
 *   12:40:25  IGNITION_ON, 99s         -> nothing recorded, all uptime
 * ------------------------------------------------------------------------ */
static void t1_reboot_inside_guard_window(void)
{
    banner("T1: a reboot inside the guard window must not cost the whole next uptime");

    /* Cold power-up: both RTC words hold garbage. The guard cannot match the magic, so
     * this boot starts normally and launders both words. */
    uint32_t guard = 0xDEADBEEFu;
    uint32_t skips = 0x5A5A5A5Au;

    CHECK(csv_bringup_decide(&guard, &skips) == CSV_BRINGUP_START,
          "a cold boot brings CSV up");
    CHECK(guard == CSV_ATTEMPT_MAGIC, "the attempt arms the guard");
    CHECK(skips == 0, "a normal start launders the cold-boot skip count");

    /* The customer rebooted at uptime ~25 s. The writer had been running ~5 s at that
     * point, well short of the 15 s stability window, so the guard is still armed. */
    CHECK(csv_guard_clear_due(5 * 1000000LL, 0) == false,
          "5 s of writer uptime does not prove stability");
    CHECK(guard == CSV_ATTEMPT_MAGIC, "the guard survives the reboot in RTC memory");

    /* The next boot finds it armed. Skipping this boot's auto-start is correct -- the
     * previous attempt genuinely did not survive. Losing the ENTIRE uptime is not: this
     * is the assertion that fails before the fix, because there was no retry at all. */
    const csv_bringup_decision_t d = csv_bringup_decide(&guard, &skips);
    CHECK(d == CSV_BRINGUP_SKIP_RETRY,
          "a first skip schedules a retry instead of forfeiting the uptime");
    CHECK(skips == 1, "the skip is counted");
    CHECK(guard == 0, "the skip disarms the guard so the next boot is clean");

    /* The delayed retry re-arms and this time the writer survives: both words clear and
     * the device is back to normal without a power cycle -- the whole point. */
    csv_bringup_arm_retry(&guard);
    CHECK(guard == CSV_ATTEMPT_MAGIC, "the retry re-arms the guard before attempting");
    CHECK(csv_guard_clear_due(16 * 1000000LL, 0) == true,
          "16 s of writer uptime proves stability");
    csv_bringup_mark_stable(&guard, &skips);
    CHECK(guard == 0 && skips == 0, "a proven writer clears the whole chain");
}

/* ---------------------------------------------------------------------------
 * T2 -- the invariant the guard exists for. Must pass before AND after the fix.
 * ------------------------------------------------------------------------ */
static void t2_boot_loop_is_still_bounded(void)
{
    banner("T2: a deterministic init crash still stops retrying");

    uint32_t guard = 0;
    uint32_t skips = 0;

    CHECK(csv_bringup_decide(&guard, &skips) == CSV_BRINGUP_START, "first attempt runs");

    /* It crashes before proving stable, so the guard stays armed. */
    CHECK(csv_bringup_decide(&guard, &skips) == CSV_BRINGUP_SKIP_RETRY,
          "the first crash earns one retry");

    /* The retry crashes too. */
    csv_bringup_arm_retry(&guard);
    CHECK(csv_bringup_decide(&guard, &skips) == CSV_BRINGUP_SKIP_FINAL,
          "the second crash ends the chain for this uptime");
    CHECK(skips == CSV_BRINGUP_MAX_SKIPS, "the bound is the skip count, not a timer");

    /* And it stays ended -- no third attempt. */
    csv_bringup_arm_retry(&guard);
    CHECK(csv_bringup_decide(&guard, &skips) == CSV_BRINGUP_SKIP_FINAL,
          "no third attempt");

    /* One stable run resets the chain, so a device that recovers is not punished. */
    csv_bringup_mark_stable(&guard, &skips);
    CHECK(csv_bringup_decide(&guard, &skips) == CSV_BRINGUP_START,
          "a proven-stable run restores normal bring-up");
}

/* ---------------------------------------------------------------------------
 * T3 -- the second latch from the same log:
 *   12:44:06  DATALOG_CLOSE (manual_stop)
 *   12:44:20  IGNITION_ON, 104s -> nothing recorded
 * ------------------------------------------------------------------------ */
static void t3_manual_stop_is_per_trip(void)
{
    banner("T3: manual Stop ends the trip, it does not disable auto-logging");

    /* The ignition goes off after a manual Stop -> the override clears itself. */
    CHECK(csv_manual_mode_next(CSV_MANUAL_OFF, true, false, false) == CSV_MANUAL_AUTO,
          "ignition-off clears a manual Stop");

    /* ...so the next key-on records, which is what did not happen at 12:44:20. */
    CHECK(csv_logging_active(false, CSV_MANUAL_AUTO, true, true) == true,
          "the next trip records normally");

    /* Guards that must hold before and after the fix. */
    CHECK(csv_manual_mode_next(CSV_MANUAL_OFF, true, true, false) == CSV_MANUAL_OFF,
          "Stop holds for the rest of the trip it stopped");
    CHECK(csv_manual_mode_next(CSV_MANUAL_OFF, false, false, false) == CSV_MANUAL_OFF,
          "no ignition edge changes nothing");
    CHECK(csv_manual_mode_next(CSV_MANUAL_OFF, true, false, true) == CSV_MANUAL_OFF,
          "a host-parked datalog is never un-parked by an ignition cycle");
    CHECK(csv_manual_mode_next(CSV_MANUAL_ON, true, false, false) == CSV_MANUAL_ON,
          "bench FORCE_ON survives an ignition cycle");
    CHECK(csv_manual_mode_next(CSV_MANUAL_AUTO, true, false, false) == CSV_MANUAL_AUTO,
          "AUTO is unchanged");
}

/* ---------------------------------------------------------------------------
 * T4 -- the gate the investigation cleared. Pinned so the extraction cannot have
 * changed it, and so a future "fix" cannot quietly redesign it.
 * ------------------------------------------------------------------------ */
static void t4_gate_truth_table_is_unchanged(void)
{
    banner("T4: the logging gate truth table is pinned");

    const int8_t modes[3] = { CSV_MANUAL_AUTO, CSV_MANUAL_ON, CSV_MANUAL_OFF };

    for (int s = 0; s < 2; s++)
    for (int m = 0; m < 3; m++)
    for (int i = 0; i < 2; i++)
    for (int e = 0; e < 2; e++)
    {
        const bool sleep_req = (s != 0);
        const int8_t mode    = modes[m];
        const bool ign       = (i != 0);
        const bool eng       = (e != 0);

        /* The expression as it stands in csv_logger.c, written out independently. */
        bool expect;
        if (sleep_req)                   { expect = false; }
        else if (mode == CSV_MANUAL_ON)  { expect = true;  }
        else if (mode == CSV_MANUAL_OFF) { expect = false; }
        else                             { expect = ign && eng; }

        CHECK(csv_logging_active(sleep_req, mode, ign, eng) == expect,
              "gate(sleep=%d mode=%d ign=%d eng=%d)", (int)sleep_req, (int)mode,
              (int)ign, (int)eng);
    }

    /* The two orderings that matter, called out so a refactor cannot invert them. */
    CHECK(csv_logging_active(true, CSV_MANUAL_ON, true, true) == false,
          "the sleep request outranks even FORCE_ON");
    CHECK(csv_logging_active(false, CSV_MANUAL_ON, false, false) == true,
          "FORCE_ON outranks the ignition gate");
}

/* ---------------------------------------------------------------------------
 * T5 -- the countdown the UI shows, and the stability boundary.
 * ------------------------------------------------------------------------ */
static void t5_countdown_and_boundaries(void)
{
    banner("T5: countdown maths and the stability boundary");

    CHECK(csv_autostart_remaining_ms(0, 10000) == 10000, "full countdown at t=0");
    CHECK(csv_autostart_remaining_ms(4000, 10000) == 6000, "counts down");
    CHECK(csv_autostart_remaining_ms(10000, 10000) == 0, "zero exactly on the deadline");
    CHECK(csv_autostart_remaining_ms(99999, 10000) == 0, "never reports negative");

    /* The uint32 ms uptime counter rolls over at ~49 days. A countdown armed just before
     * the rollover must keep counting down, not jump to ~49 days remaining -- which is
     * what an unsigned subtraction would report. */
    CHECK(csv_autostart_remaining_ms(0xFFFFFF00u, 0xFFFFFF00u + 5000u) == 5000,
          "a countdown straddling the ms rollover still counts down");
    CHECK(csv_autostart_remaining_ms(0xFFFFFF00u + 6000u, 0xFFFFFF00u + 5000u) == 0,
          "and still expires across the rollover");

    CHECK(csv_guard_clear_due(CSV_GUARD_STABLE_US, 0) == false,
          "not yet proven exactly on the boundary");
    CHECK(csv_guard_clear_due(CSV_GUARD_STABLE_US + 1, 0) == true,
          "proven one microsecond later");
    CHECK(csv_guard_clear_due(CSV_GUARD_STABLE_US + 5, 5) == false,
          "the window is measured from task start, not from boot");
}

int main(void)
{
    printf("# csv_logger bring-up host tests\n");
    t1_reboot_inside_guard_window();
    t2_boot_loop_is_still_bounded();
    t3_manual_stop_is_per_trip();
    t4_gate_truth_table_is_unchanged();
    t5_countdown_and_boundaries();

    printf("1..%d\n", g_checks);
    if (g_failures != 0)
    {
        printf("# FAILED %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("# ok - all %d checks passed\n", g_checks);
    return 0;
}
