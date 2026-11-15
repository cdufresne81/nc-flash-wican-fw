/* Host tests for the external-client rule (main/ext_client_logic.h) -- an ELM327 app is using
 * the car, so the standalone datalogger steps aside, and NC Flash still wins over both.
 *
 *   X1  A device that never had a client is not "active", including right after boot.
 *   X2  A byte starts the hold, and it lasts exactly the hold window from the LAST byte.
 *   X3  The hold lapses by itself: nothing has to release it.
 *   X4  The arithmetic is right across the 32-bit millisecond rollover.
 *   X5  Housekeeping drops a lapsed hold, never a live one, and a stale stamp cannot come back
 *       to life a full clock period later.
 *   X6  NC Flash wins: flash, claim, pause and sleep each refuse the client; flash is named first.
 *   X7  Every verdict has a stable name.
 *   X8  Command counting: one per carriage return, and NULL/empty are zero.
 *
 * Build + run: tools/hosttest/run.sh
 */

#include <stdio.h>
#include <string.h>

#include "ext_client_logic.h"

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

static ext_client_verdict_t gate(bool flash, bool claim, bool park, bool sleep)
{
    ext_client_gate_in_t in = { flash, claim, park, sleep };
    return ext_client_gate(&in);
}

int main(void)
{
    printf("# X1 no client yet\n");
    {
        ext_client_hold_t h = { false, 0 };
        CHECK(!ext_client_active(&h, 0),    "boot, clock 0");
        CHECK(!ext_client_active(&h, 5000), "boot + 5 s, stamp still 0");
        CHECK(ext_client_idle_ms(&h, 5000) == UINT32_MAX, "idle is 'never'");
        CHECK(!ext_client_active(NULL, 0),  "NULL is inactive");
        CHECK(ext_client_idle_ms(NULL, 0) == UINT32_MAX, "NULL idle is 'never'");
    }

    printf("# X2 a byte starts the hold, measured from the last byte\n");
    {
        ext_client_hold_t h = { false, 0 };
        ext_client_touch(&h, 1000);
        CHECK(ext_client_active(&h, 1000), "active at the byte");
        CHECK(ext_client_active(&h, 1000 + EXT_CLIENT_HOLD_MS - 1), "active 1 ms before the window ends");
        CHECK(!ext_client_active(&h, 1000 + EXT_CLIENT_HOLD_MS),    "inactive exactly at the window");
        CHECK(ext_client_idle_ms(&h, 1250) == 250, "idle counts from the byte");
        ext_client_touch(&h, 9000);
        CHECK(ext_client_active(&h, 9000 + EXT_CLIENT_HOLD_MS - 1), "a later byte restarts the window");
        ext_client_touch(NULL, 1);   /* must not crash */
    }

    printf("# X3 it lapses by itself\n");
    {
        ext_client_hold_t h = { false, 0 };
        ext_client_touch(&h, 100);
        CHECK(!ext_client_active(&h, 100 + EXT_CLIENT_HOLD_MS + 1), "no release call needed");
        CHECK(!ext_client_active(&h, 100 + 3600000U),               "still inactive an hour later");
    }

    printf("# X4 rollover\n");
    {
        ext_client_hold_t h = { false, 0 };
        const uint32_t near_wrap = UINT32_MAX - 2000U;
        ext_client_touch(&h, near_wrap);
        CHECK(ext_client_active(&h, near_wrap + 5000U),  "active across the wrap (now < stamp)");
        CHECK(ext_client_idle_ms(&h, near_wrap + 5000U) == 5000U, "idle across the wrap");
        CHECK(!ext_client_active(&h, near_wrap + EXT_CLIENT_HOLD_MS), "lapses on time across the wrap");
    }

    printf("# X5 housekeeping\n");
    {
        ext_client_hold_t h = { false, 0 };
        CHECK(!ext_client_expire(&h, 123), "nothing to expire");
        ext_client_touch(&h, 1000);
        CHECK(!ext_client_expire(&h, 1000 + EXT_CLIENT_HOLD_MS - 1), "a live hold is never dropped");
        CHECK(h.seen, "still seen");
        CHECK(ext_client_expire(&h, 1000 + EXT_CLIENT_HOLD_MS), "a lapsed hold is dropped");
        CHECK(!h.seen, "flag cleared");
        CHECK(!ext_client_expire(&h, 1000 + EXT_CLIENT_HOLD_MS + 1), "and only once");
        /* A full clock period later `now - stamp` is small again. Without the flag this would
         * bench the logger for ten seconds with nobody connected. */
        CHECK(!ext_client_active(&h, 1000 + 5), "stale stamp does not revive after the wrap");
        CHECK(!ext_client_expire(NULL, 0), "NULL is a no-op");
    }

    printf("# X6 NC Flash wins\n");
    CHECK(gate(false, false, false, false) == EXT_CLIENT_ADMIT, "idle: admit");
    CHECK(gate(false, true,  false, false) == EXT_CLIENT_REFUSE_SESSION, "host claim refuses");
    CHECK(gate(false, false, true,  false) == EXT_CLIENT_REFUSE_SESSION, "host pause refuses");
    CHECK(gate(false, false, false, true)  == EXT_CLIENT_REFUSE_SLEEP,   "sleep teardown refuses");
    for (int m = 0; m < 8; m++)
    {
        bool claim = m & 1, park = m & 2, sleep = m & 4;
        CHECK(gate(true, claim, park, sleep) == EXT_CLIENT_REFUSE_FLASH,
              "flash + (claim=%d park=%d sleep=%d) must be FLASH", claim, park, sleep);
    }
    CHECK(gate(false, true, false, true) == EXT_CLIENT_REFUSE_SESSION, "session is named before sleep");
    CHECK(ext_client_gate(NULL) == EXT_CLIENT_ADMIT, "NULL admits (nothing known to hold the bus)");

    printf("# X7 names\n");
    CHECK(strcmp(ext_client_verdict_name(EXT_CLIENT_ADMIT),          "admit")   == 0, "admit");
    CHECK(strcmp(ext_client_verdict_name(EXT_CLIENT_REFUSE_FLASH),   "flash")   == 0, "flash");
    CHECK(strcmp(ext_client_verdict_name(EXT_CLIENT_REFUSE_SESSION), "session") == 0, "session");
    CHECK(strcmp(ext_client_verdict_name(EXT_CLIENT_REFUSE_SLEEP),   "sleep")   == 0, "sleep");
    CHECK(ext_client_verdict_name((ext_client_verdict_t)99) != NULL, "unknown is not NULL");

    printf("# X8 command counting\n");
    CHECK(ext_client_count_commands((const uint8_t *)"ATZ\r", 4) == 1, "one command");
    CHECK(ext_client_count_commands((const uint8_t *)"ATE0\rATL0\r010C\r", 15) == 3, "three in one chunk");
    CHECK(ext_client_count_commands((const uint8_t *)"010", 3) == 0, "a partial line is not a command");
    CHECK(ext_client_count_commands((const uint8_t *)"\r\n", 2) == 1, "LF does not count");
    CHECK(ext_client_count_commands(NULL, 5) == 0, "NULL");
    CHECK(ext_client_count_commands((const uint8_t *)"x", 0) == 0, "empty");

    if (g_failures)
    {
        printf("FAIL: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("ok: ext_client %d checks\n", g_checks);
    return 0;
}
