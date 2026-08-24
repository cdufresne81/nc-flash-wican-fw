/* Host tests for poll_log's RECORDING gate -- the "is the engine actually turning" answer the
 * CSV writer trusts.
 *
 * These exist because of two bugs that reached a bench and would have reached a car, neither of
 * which any existing test could see:
 *
 *   T1  An open CSV session force-set the gate open. The gate then answered "the engine is
 *       running" with the meaning "we are already recording" -- a circle with no exit. On a
 *       bench with the engine stopped, one junk session held it open for twelve hours and wrote
 *       347 MB of a stationary car.
 *
 *   T2  A missing rpm ANSWER was treated as a missing rpm CHANNEL, so the gate fell straight
 *       back to voltage. Every pause longer than the freshness window -- a host park, a bus
 *       claim, a dead-man reaper resume -- bought a voltage-only opening on the very next pass,
 *       before the next sweep could deliver the fresh zero. This is what restarted the
 *       datalogger by itself after every NC Flash session.
 *
 *   T8  The same confusion at boot, where no sample has landed yet at all. The gate opened on
 *       voltage and wrote a junk session on EVERY boot -- on a bench, and equally on a car
 *       rebooted soon after a drive, where the battery still reads 13.2-13.5 V. The gate's rpm
 *       input is a CONFIG fact for exactly this reason.
 *
 * T3-T7 are the invariants the fix must not break. T3 and T4 in particular: the added strictness
 * must never be able to lose a real drive, so a dead channel and a never-configured channel both
 * still record.
 *
 * Build + run: tools/hosttest/run.sh
 */

#include <stdio.h>
#include <string.h>

#include "poll_gate_logic.h"

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

/* The bench: 13.9 V from a supply, engine_volt 13.2, ECU answering. */
#define BENCH_VOLTS   13.9f
#define GATE_VOLT_ON  13.2f
#define HYST_V        0.3f
#define OFF_MS        3000u

static poll_gate_in_t base_in(int64_t now_us)
{
    poll_gate_in_t in;
    memset(&in, 0, sizeof(in));
    in.now_us          = now_us;
    in.session_active  = false;
    in.ecu_answering   = true;
    in.have_volts      = true;
    in.volts           = BENCH_VOLTS;
    in.gate_volt_on    = GATE_VOLT_ON;
    in.hyst_v          = HYST_V;
    in.off_debounce_ms = OFF_MS;
    in.rpm_configured  = true;
    in.rpm_known       = true;
    in.rpm_running     = false;   /* bench PCM: a fresh, honest zero */
    return in;
}

/* ---------------------------------------------------------------------------
 * T1 -- the bench stuck-open bug. A fresh RPM 0 at 13.9 V closes the gate and keeps it
 * closed, and an open CSV session must NOT be able to reopen it.
 * ------------------------------------------------------------------------ */
static void t1_session_cannot_open_the_gate(void)
{
    banner("T1 session forces the sweep RATE, never the engine ANSWER");

    poll_gate_state_t st = { .gate_open = true, .low_since_us = 0, .stale_blocks = 0 };
    poll_gate_out_t   out;
    int64_t t = 1000000;

    /* Fresh zero rpm: the debounce starts, then expires, and the gate closes. */
    poll_gate_in_t in = base_in(t);
    poll_gate_step(&st, &in, &out);
    CHECK(out.gate_open, "first low pass only starts the debounce");

    t += (int64_t)OFF_MS * 1000 + 1000;
    in = base_in(t);
    poll_gate_step(&st, &in, &out);
    CHECK(out.event == POLL_GATE_CLOSED, "gate closes after the off debounce");
    CHECK(!out.gate_open, "gate is closed");
    CHECK(out.closed_on_rpm, "closed because rpm said stopped, not because of volts");

    /* Now open a CSV session and keep hammering. The gate must stay shut for as long as the
     * engine is stopped -- this is the twelve-hour bug. */
    for (int i = 0; i < 500; i++)
    {
        t += 40000;
        in = base_in(t);
        in.session_active = true;
        poll_gate_step(&st, &in, &out);
        if (out.gate_open)
            break;
    }
    CHECK(!out.gate_open, "an open session NEVER reopens the engine gate");
    CHECK(out.fast_sweep, "...but it does keep the sweep at full rate");

    /* And with no session, a closed gate means a slow sweep. */
    t += 40000;
    in = base_in(t);
    poll_gate_step(&st, &in, &out);
    CHECK(!out.fast_sweep, "no session and a closed gate -> watch sweep");
}

/* ---------------------------------------------------------------------------
 * T2 -- the reaper-reopen bug. Stale-but-seen rpm must not open the gate on voltage; the
 * fresh zero arriving on the next pass keeps it closed. No session, not even a junk file.
 * ------------------------------------------------------------------------ */
static void t2_stale_rpm_does_not_open_the_gate(void)
{
    banner("T2 a stale rpm sample is not a missing rpm channel");

    poll_gate_state_t st = { .gate_open = false, .low_since_us = 0, .stale_blocks = 0 };
    poll_gate_out_t   out;
    int64_t t = 1000000;

    /* First pass after a resume: the channel has answered before, but not recently. */
    poll_gate_in_t in = base_in(t);
    in.rpm_known = false;
    poll_gate_step(&st, &in, &out);
    CHECK(out.event == POLL_GATE_WAIT_RPM, "gate waits for a fresh sample instead of opening");
    CHECK(!out.gate_open, "gate stays closed while it waits");

    /* Next sweep delivers the fresh zero. */
    t += 40000;
    in = base_in(t);          /* rpm_known true, rpm_running false */
    poll_gate_step(&st, &in, &out);
    CHECK(!out.gate_open, "a fresh zero keeps the gate closed -- no session, no junk file");

    /* And the allowance is given back, so the NEXT pause gets its full wait again. */
    CHECK(st.stale_blocks == 0, "the confirm counter resets once a fresh sample lands");
}

/* ---------------------------------------------------------------------------
 * T3 -- mid-drive resume. The same wait must cost a real car almost nothing: one pass, then
 * the fresh rpm opens it.
 * ------------------------------------------------------------------------ */
static void t3_mid_drive_resume_opens_immediately(void)
{
    banner("T3 a mid-drive resume loses at most one pass");

    poll_gate_state_t st = { .gate_open = false, .low_since_us = 0, .stale_blocks = 0 };
    poll_gate_out_t   out;
    int64_t t = 1000000;

    poll_gate_in_t in = base_in(t);
    in.rpm_known = false;
    poll_gate_step(&st, &in, &out);
    CHECK(!out.gate_open, "blocked for one pass");

    t += 40000;
    in = base_in(t);
    in.rpm_running = true;    /* 3000 rpm */
    poll_gate_step(&st, &in, &out);
    CHECK(out.event == POLL_GATE_OPENED, "the fresh running sample opens it on the next pass");
    CHECK(out.gate_open && out.fast_sweep, "open and sweeping fast");
}

/* ---------------------------------------------------------------------------
 * T4 -- the two fail-open paths. A dead channel opens after exactly
 * POLLLOG_RPM_CONFIRM_SWEEPS blocked passes; a never-configured channel opens immediately,
 * exactly as it ships. Losing a real drive must be impossible.
 * ------------------------------------------------------------------------ */
static void t4_fail_open_paths(void)
{
    banner("T4 a dead rpm channel still records the drive");

    poll_gate_state_t st = { .gate_open = false, .low_since_us = 0, .stale_blocks = 0 };
    poll_gate_out_t   out;
    int64_t t = 1000000;
    int blocked = 0;

    for (int i = 0; i < 10; i++)
    {
        poll_gate_in_t in = base_in(t);
        in.rpm_known = false;      /* seen once, then dead forever */
        poll_gate_step(&st, &in, &out);
        if (out.gate_open)
            break;
        blocked++;
        t += 1000000;              /* watch cadence */
    }
    CHECK(blocked == (int)POLLLOG_RPM_CONFIRM_SWEEPS,
          "exactly %u blocked passes, got %d", (unsigned)POLLLOG_RPM_CONFIRM_SWEEPS, blocked);
    CHECK(out.gate_open, "then it opens on voltage alone -- the shipped fail-open survives");

    banner("T4b a config with no rpm channel CONFIGURED keeps today's voltage-only gate, no delay");

    poll_gate_state_t st2 = { .gate_open = false, .low_since_us = 0, .stale_blocks = 0 };
    poll_gate_in_t in2 = base_in(2000000);
    in2.rpm_configured = false;
    in2.rpm_known      = false;
    poll_gate_step(&st2, &in2, &out);
    CHECK(out.event == POLL_GATE_OPENED, "opens on the very first pass, no wait at all");
}

/* ---------------------------------------------------------------------------
 * T5 -- a pause must never end a live trip. Gate open, rpm goes stale, volts fine: the gate
 * stays open indefinitely.
 * ------------------------------------------------------------------------ */
static void t5_staleness_never_ends_a_trip(void)
{
    banner("T5 a paused poller cannot end a live trip");

    poll_gate_state_t st = { .gate_open = true, .low_since_us = 0, .stale_blocks = 0 };
    poll_gate_out_t   out;
    int64_t t = 1000000;

    for (int i = 0; i < 200; i++)   /* ~80 s of staleness, far past the 3 s debounce */
    {
        poll_gate_in_t in = base_in(t);
        in.rpm_known   = false;
        in.rpm_running = false;
        poll_gate_step(&st, &in, &out);
        CHECK(out.gate_open, "gate stays open through staleness (pass %d)", i);
        if (!out.gate_open) break;
        t += 400000;
    }

    /* rpm comes back and says stopped: NOW it closes, after the normal debounce. */
    poll_gate_in_t in = base_in(t);
    poll_gate_step(&st, &in, &out);
    t += (int64_t)OFF_MS * 1000 + 1000;
    in = base_in(t);
    poll_gate_step(&st, &in, &out);
    CHECK(out.event == POLL_GATE_CLOSED, "a fresh stopped sample closes it normally");
}

/* ---------------------------------------------------------------------------
 * T6 -- an rpm channel flip-flopping fresh/stale must NOT be able to hold the gate open
 * against a stopped engine. Staleness may hold the debounce; it may never restart it.
 * ------------------------------------------------------------------------ */
static void t6_flip_flop_still_closes(void)
{
    banner("T6 a flip-flopping rpm channel cannot keep a stopped engine's gate open");

    poll_gate_state_t st = { .gate_open = true, .low_since_us = 0, .stale_blocks = 0 };
    poll_gate_out_t   out;
    int64_t t = 1000000;
    int closed_at = -1;

    for (int i = 0; i < 60; i++)
    {
        /* 2.0 s of fresh zeros, then 0.5 s of staleness, repeating. */
        poll_gate_in_t in = base_in(t);
        in.rpm_known = ((i % 5) != 4);
        poll_gate_step(&st, &in, &out);
        if (out.event == POLL_GATE_CLOSED) { closed_at = i; break; }
        t += 500000;
    }
    CHECK(closed_at >= 0, "the gate closes despite the flip-flop");
}

/* ---------------------------------------------------------------------------
 * T7 -- the quiesce override. The ECU is off the bus, so there is nothing to debounce.
 * ------------------------------------------------------------------------ */
static void t7_force_close(void)
{
    banner("T7 force-close leaves no state behind for the next evaluation");

    poll_gate_state_t st = { .gate_open = true, .low_since_us = 12345, .stale_blocks = 2 };
    poll_gate_force_close(&st);
    CHECK(!st.gate_open, "closed");
    CHECK(st.low_since_us == 0, "debounce cleared");
    CHECK(st.stale_blocks == 0, "confirm counter cleared");
}

/* ---------------------------------------------------------------------------
 * T8 -- the boot junk session. A configured rpm channel that has NEVER delivered a sample is
 * still a channel: the gate must wait for it, not open on voltage. This is the bench
 * regression, and it pins the reason the input is a config fact and not "has a sample arrived".
 * ------------------------------------------------------------------------ */
static void t8_boot_waits_for_the_first_sample(void)
{
    banner("T8 a configured rpm channel with no sample YET does not open the gate");

    poll_gate_state_t st = { .gate_open = false, .low_since_us = 0, .stale_blocks = 0 };
    poll_gate_out_t   out;
    int64_t t = 4163000;   /* the bench: ECU answering 4.163 s after boot */

    poll_gate_in_t in = base_in(t);
    in.rpm_known   = false;   /* nothing has stamped rpm yet this uptime */
    in.rpm_running = false;
    poll_gate_step(&st, &in, &out);
    CHECK(out.event == POLL_GATE_WAIT_RPM, "boot pass waits instead of opening on voltage");
    CHECK(!out.gate_open, "no gate, so no DATALOG_OPEN on every boot");

    /* The first sweep lands and says the engine is stopped. It must STAY closed. */
    t += 40000;
    in = base_in(t);
    poll_gate_step(&st, &in, &out);
    CHECK(!out.gate_open, "the first real sample keeps it closed");
    CHECK(out.event != POLL_GATE_OPENED, "the gate never opened at any point during boot");
}

int main(void)
{
    printf("# poll_gate_logic host tests\n");
    t1_session_cannot_open_the_gate();
    t2_stale_rpm_does_not_open_the_gate();
    t3_mid_drive_resume_opens_immediately();
    t4_fail_open_paths();
    t5_staleness_never_ends_a_trip();
    t6_flip_flop_still_closes();
    t7_force_close();
    t8_boot_waits_for_the_first_sample();

    printf("1..%d\n", g_checks);
    if (g_failures)
    {
        printf("FAILED %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("ok - all %d checks passed\n", g_checks);
    return 0;
}
