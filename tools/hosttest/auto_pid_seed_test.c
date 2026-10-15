/* Host tests for the default-sensor-list seed -- when the boot code may write the NC
 * default list over /littlefs/auto_pid.json.
 *
 *   S1  No file: write the default.
 *   S2  A filesystem error is never "missing": keep.
 *   S3  An empty or broken file: replace (with a backup).
 *   S4  A file our Logger page saved ("format" or "calculated") is kept, even empty.
 *   S5  An official-firmware file with no sensors is replaced (with a backup).
 *   S6  An official-firmware file WITH sensors is kept.
 *   S7  An oversized file is kept.
 *   S8  Every action has a stable name.
 *
 * Build + run: tools/hosttest/run.sh
 */

#include <stdio.h>
#include <string.h>

#include "auto_pid_seed_logic.h"

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

/* A parsed file that exists, with the given ownership keys and sensor count. */
static auto_pid_seed_t file(bool has_format, bool has_calculated, int sensors)
{
    auto_pid_seed_in_t in = { .exists = true, .parsed = true, .has_format = has_format,
                              .has_calculated = has_calculated, .sensors = sensors };
    return auto_pid_seed_decide(&in);
}

int main(void)
{
    printf("# S1 no file\n");
    auto_pid_seed_in_t missing = { .exists = false };
    CHECK(auto_pid_seed_decide(&missing) == AUTO_PID_SEED_WRITE, "missing file -> write");

    printf("# S2 filesystem error\n");
    auto_pid_seed_in_t fs_err = { .exists = false, .stat_failed = true };
    CHECK(auto_pid_seed_decide(&fs_err) == AUTO_PID_SEED_KEEP, "stat error -> keep");
    CHECK(auto_pid_seed_decide(NULL) == AUTO_PID_SEED_KEEP, "NULL input -> keep");

    printf("# S3 empty or broken\n");
    auto_pid_seed_in_t bad = { .exists = true, .parsed = false };
    CHECK(auto_pid_seed_decide(&bad) == AUTO_PID_SEED_REPLACE_BAD, "unparseable -> replace_bad");

    printf("# S4 saved by this firmware\n");
    CHECK(file(true,  false, 0) == AUTO_PID_SEED_KEEP, "format, empty -> keep");
    CHECK(file(false, true,  0) == AUTO_PID_SEED_KEEP, "calculated, empty -> keep");
    CHECK(file(true,  true,  0) == AUTO_PID_SEED_KEEP, "both, empty -> keep");
    CHECK(file(true,  true, 29) == AUTO_PID_SEED_KEEP, "both, full -> keep");

    printf("# S5 official firmware, no sensors\n");
    CHECK(file(false, false, 0) == AUTO_PID_SEED_REPLACE_OFFICIAL, "official, empty -> replace_official");

    printf("# S6 official firmware, with sensors\n");
    CHECK(file(false, false, 1) == AUTO_PID_SEED_KEEP, "official, 1 sensor -> keep");

    printf("# S7 oversized\n");
    auto_pid_seed_in_t big = { .exists = true, .too_big = true };
    CHECK(auto_pid_seed_decide(&big) == AUTO_PID_SEED_KEEP, "too big -> keep");

    printf("# S8 names\n");
    CHECK(strcmp(auto_pid_seed_name(AUTO_PID_SEED_KEEP), "keep") == 0, "keep name");
    CHECK(strcmp(auto_pid_seed_name(AUTO_PID_SEED_WRITE), "write") == 0, "write name");
    CHECK(strcmp(auto_pid_seed_name(AUTO_PID_SEED_REPLACE_BAD), "replace_bad") == 0, "replace_bad name");
    CHECK(strcmp(auto_pid_seed_name(AUTO_PID_SEED_REPLACE_OFFICIAL), "replace_official") == 0, "replace_official name");

    if (g_failures)
    {
        printf("FAIL: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("ok: auto_pid_seed %d checks\n", g_checks);
    return 0;
}
