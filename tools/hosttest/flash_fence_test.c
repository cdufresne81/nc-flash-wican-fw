/* Host tests for the ECU-flash fence (#145) -- whether a reboot, update, config save or SD-card
 * change may go ahead while NC Flash is working on the car.
 *
 *   F1  A running flash refuses everything, whatever the claim says.
 *   F2  A live claim (lease valid, or its socket still open) is a session.
 *   F3  A claim a dead host left behind (lease expired, socket gone) fences NOTHING -- otherwise a
 *       crashed NC Flash locks the owner out of Reboot until the key turns off.
 *   F4  SD-card operations are only refused by a running flash, never by a session: NC Flash
 *       uploads its staged ROM while it holds the claim.
 *   F5  The reboot timer waits for a flash, and only for a flash.
 *   F6  A flash may not start while a reboot is pending or a firmware update is running.
 *   F7  Every level has a stable name, and every refusing level has a message.
 *
 * Build + run: tools/hosttest/run.sh
 */

#include <stdio.h>
#include <string.h>

#include "flash_fence_logic.h"

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

static flash_fence_t eval(bool flash, bool raised, bool expired, bool alive)
{
    flash_fence_in_t in = { flash, raised, expired, alive };
    return flash_fence_eval(&in);
}

int main(void)
{
    printf("# F1 a running flash refuses everything\n");
    for (int m = 0; m < 8; m++)
    {
        bool raised = m & 1, expired = m & 2, alive = m & 4;
        CHECK(eval(true, raised, expired, alive) == FLASH_FENCE_FLASHING,
              "flash + claim(raised=%d expired=%d alive=%d) must be FLASHING", raised, expired, alive);
    }

    printf("# F2 a live claim is a session\n");
    CHECK(eval(false, true, false, false) == FLASH_FENCE_SESSION, "lease valid, socket gone");
    CHECK(eval(false, true, false, true)  == FLASH_FENCE_SESSION, "lease valid, socket open");
    CHECK(eval(false, true, true,  true)  == FLASH_FENCE_SESSION, "lease expired, socket still open");

    printf("# F3 a dead host's leftover claim fences nothing\n");
    CHECK(eval(false, true,  true,  false) == FLASH_FENCE_CLEAR, "lease expired and socket gone");
    CHECK(eval(false, false, false, false) == FLASH_FENCE_CLEAR, "idle");
    CHECK(eval(false, false, true,  true)  == FLASH_FENCE_CLEAR, "no claim raised: lease fields ignored");
    CHECK(flash_fence_eval(NULL) == FLASH_FENCE_CLEAR, "NULL input");

    printf("# F4 SD operations: flash only\n");
    CHECK(flash_fence_for_sd(FLASH_FENCE_FLASHING) == FLASH_FENCE_FLASHING, "flash refuses SD ops");
    CHECK(flash_fence_for_sd(FLASH_FENCE_SESSION)  == FLASH_FENCE_CLEAR,    "session must not refuse SD ops");
    CHECK(flash_fence_for_sd(FLASH_FENCE_CLEAR)    == FLASH_FENCE_CLEAR,    "clear stays clear");

    printf("# F5 the reboot timer waits for a flash only\n");
    CHECK(!flash_fence_reboot_may_fire(FLASH_FENCE_FLASHING), "must wait while flashing");
    CHECK(flash_fence_reboot_may_fire(FLASH_FENCE_SESSION),   "a session must not hold a reboot forever");
    CHECK(flash_fence_reboot_may_fire(FLASH_FENCE_CLEAR),     "clear fires");

    printf("# F6 a flash may not start into a reboot or an update\n");
    CHECK(flash_fence_write_may_start(false, false),  "idle: may start");
    CHECK(!flash_fence_write_may_start(true,  false), "reboot pending: refuse");
    CHECK(!flash_fence_write_may_start(false, true),  "OTA running: refuse");
    CHECK(!flash_fence_write_may_start(true,  true),  "both: refuse");

    printf("# F7 names and messages\n");
    CHECK(strcmp(flash_fence_name(FLASH_FENCE_CLEAR),    "clear")    == 0, "clear name");
    CHECK(strcmp(flash_fence_name(FLASH_FENCE_SESSION),  "session")  == 0, "session name");
    CHECK(strcmp(flash_fence_name(FLASH_FENCE_FLASHING), "flashing") == 0, "flashing name");
    CHECK(flash_fence_message(FLASH_FENCE_FLASHING)[0] != '\0', "flashing has a message");
    CHECK(flash_fence_message(FLASH_FENCE_SESSION)[0]  != '\0', "session has a message");

    if (g_failures)
    {
        printf("FAIL: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("ok: flash_fence %d checks\n", g_checks);
    return 0;
}
