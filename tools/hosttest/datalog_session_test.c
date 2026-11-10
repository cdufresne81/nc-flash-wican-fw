/* Host tests for the NC Flash logging session (docs/internals/datalog_session.md) -- the
 * temporary channel list the PC hands the adapter for one logging run.
 *
 *   D1  ttl_s: the default when absent, clamped to 10..120 when given.
 *   D2  A session may not begin while anything else owns the bus, or with no poller to apply it.
 *   D3  Begin issues a non-zero token and saves the owner's manual mode.
 *   D4  A second begin is the same run: new token, new lease, the FIRST saved mode kept --
 *       otherwise the session would hand back its own forced-on and latch the logger on.
 *   D5  Only the current token renews or ends. Token 0 (no token sent) never matches.
 *   D6  A lease that is not renewed runs out, including across the 32-bit ms rollover.
 *   D7  Ending records why, hands back the saved mode, and retires the token for good.
 *   D8  Reason names are the HTTP contract.
 *   D9  The nesting bound: a real list passes, a nesting bomb does not, brackets inside
 *       strings do not count.
 *
 * Build + run: tools/hosttest/run.sh
 */

#include <stdio.h>
#include <string.h>

#include "datalog_session_logic.h"

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

/* The CSV manual modes, as csv_bringup_logic.h numbers them. Opaque to the logic under test. */
enum { MODE_AUTO = 0, MODE_ON = 1, MODE_OFF = 2 };

static dls_begin_check_t check(bool fence_clear, bool parked, bool claimed, bool sleeping,
                               bool poll_active)
{
    const dls_begin_in_t in = { fence_clear, parked, claimed, sleeping, poll_active };
    return dls_begin_check(&in);
}

static bool depth_ok(const char *s, int max)
{
    return dls_json_depth_ok(s, strlen(s), max);
}

int main(void)
{
    printf("# D1 ttl_s default and clamp\n");
    CHECK(dls_clamp_ttl_s(false, 0)   == 30,  "absent -> 30");
    CHECK(dls_clamp_ttl_s(false, 999) == 30,  "absent ignores the value");
    CHECK(dls_clamp_ttl_s(true, 0)    == 10,  "0 (or unparseable) -> 10");
    CHECK(dls_clamp_ttl_s(true, 9)    == 10,  "below the floor");
    CHECK(dls_clamp_ttl_s(true, 10)   == 10,  "the floor");
    CHECK(dls_clamp_ttl_s(true, 45)   == 45,  "in range is kept");
    CHECK(dls_clamp_ttl_s(true, 120)  == 120, "the ceiling");
    CHECK(dls_clamp_ttl_s(true, 121)  == 120, "above the ceiling");
    CHECK(dls_clamp_ttl_s(true, 0xFFFFFFFFu) == 120, "a huge value cannot overflow the lease maths");

    printf("# D2 begin is refused while the bus is owned\n");
    CHECK(check(true,  false, false, false, true)  == DLS_BEGIN_OK,       "idle adapter");
    CHECK(check(false, false, false, false, true)  == DLS_BEGIN_BUSY,     "flash fence not clear");
    CHECK(check(true,  true,  false, false, true)  == DLS_BEGIN_BUSY,     "host datalog park");
    CHECK(check(true,  false, true,  false, true)  == DLS_BEGIN_BUSY,
          "a raw bus claim, even one the fence no longer counts, still parks the poller");
    CHECK(check(true,  false, false, true,  true)  == DLS_BEGIN_BUSY,     "going to sleep");
    CHECK(check(true,  false, false, false, false) == DLS_BEGIN_INACTIVE, "no poller");
    CHECK(check(false, false, false, false, false) == DLS_BEGIN_BUSY,     "busy wins over inactive");
    CHECK(dls_begin_check(NULL) == DLS_BEGIN_BUSY, "NULL input refuses");
    CHECK(dls_begin_refusal_msg(DLS_BEGIN_BUSY)[0] != '\0',     "busy has a message");
    CHECK(dls_begin_refusal_msg(DLS_BEGIN_INACTIVE)[0] != '\0', "inactive has a message");
    CHECK(dls_begin_refusal_msg(DLS_BEGIN_OK)[0] == '\0',       "ok has none");
    /* The messages go into a JSON string unescaped. */
    CHECK(strpbrk(dls_begin_refusal_msg(DLS_BEGIN_BUSY), "\"\\\n") == NULL,     "busy is JSON-safe");
    CHECK(strpbrk(dls_begin_refusal_msg(DLS_BEGIN_INACTIVE), "\"\\\n") == NULL, "inactive is JSON-safe");

    printf("# D3 begin\n");
    dls_state_t st;
    memset(&st, 0, sizeof(st));
    CHECK(!dls_token_matches(&st, 0), "no session: token 0 does not match");
    CHECK(!dls_expired(&st, 123456),  "no session: nothing to expire");
    CHECK(dls_remaining_ms(&st, 123456) == 0, "no session: nothing remaining");
    CHECK(dls_end_name(st.last_end)[0] == '\0', "no session yet: last_end is empty");

    const uint32_t t1 = dls_begin(&st, 1000, 30, MODE_AUTO, 0);
    CHECK(t1 != 0, "a zero seed still yields a non-zero token");
    CHECK(st.active, "active after begin");
    CHECK(st.saved_mode == MODE_AUTO, "the owner's mode is saved");
    CHECK(dls_remaining_ms(&st, 1000) == 30000, "full lease at begin");
    CHECK(dls_remaining_ms(&st, 11000) == 20000, "counts down");

    printf("# D4 a second begin keeps the first saved mode\n");
    const uint32_t t2 = dls_begin(&st, 5000, 60, MODE_ON, t1);   /* seed collides on purpose */
    CHECK(t2 != 0 && t2 != t1, "a replace never reissues the token the host already holds");
    CHECK(st.saved_mode == MODE_AUTO, "the mode the session forced is NOT saved over the owner's");
    CHECK(dls_remaining_ms(&st, 5000) == 60000, "the lease restarts with the new ttl");

    printf("# D5 only the current token counts\n");
    CHECK(!dls_token_matches(&st, t1), "the replaced token is stale");
    CHECK(!dls_token_matches(&st, 0),  "token 0 never matches");
    CHECK(dls_token_matches(&st, t2),  "the current token matches");
    CHECK(!dls_keepalive(&st, t1, 6000), "a stale keepalive is refused");
    CHECK(dls_remaining_ms(&st, 6000) == 59000, "and renews nothing");
    CHECK(dls_keepalive(&st, t2, 50000), "the current token renews");
    CHECK(dls_remaining_ms(&st, 50000) == 60000, "by the session's own ttl");

    printf("# D6 expiry\n");
    CHECK(!dls_expired(&st, 50000 + 59999), "one ms before the deadline");
    CHECK(dls_expired(&st, 50000 + 60000),  "on the deadline");
    CHECK(dls_expired(&st, 50000 + 3600000u), "an hour later");
    CHECK(dls_remaining_ms(&st, 50000 + 3600000u) == 0, "remaining never goes negative");
    CHECK(dls_keepalive(&st, t2, 50000 + 61000), "a late keepalive from the live host still renews");
    CHECK(!dls_expired(&st, 50000 + 61000), "and the session is alive again");
    {
        dls_state_t w;
        memset(&w, 0, sizeof(w));
        const uint32_t near = 0xFFFFFF00u;   /* 256 ms before the uint32 ms rollover */
        dls_begin(&w, near, 10, MODE_AUTO, 7);
        CHECK(!dls_expired(&w, near + 9999u), "a lease straddling the rollover is still alive");
        CHECK(dls_remaining_ms(&w, near + 4000u) == 6000, "and counts down across it");
        CHECK(dls_expired(&w, near + 10000u), "and still runs out");
    }

    printf("# D7 end\n");
    CHECK(dls_end(&st, DLS_END_EXPIRED) == MODE_AUTO, "end hands back the saved mode");
    CHECK(!st.active, "inactive after end");
    CHECK(st.last_end == DLS_END_EXPIRED, "the reason is recorded");
    CHECK(!dls_token_matches(&st, t2), "the ended session's token is stale");
    CHECK(!dls_keepalive(&st, t2, 1), "and cannot revive it");
    CHECK(dls_remaining_ms(&st, 1) == 0, "nothing remaining after end");
    const uint32_t t3 = dls_begin(&st, 200000, 30, MODE_OFF, t2);
    CHECK(t3 != t2, "the next session never reuses the last token");
    CHECK(st.saved_mode == MODE_OFF, "a NEW session saves the mode afresh");
    CHECK(st.last_end == DLS_END_NONE, "and clears the last end reason");
    CHECK(dls_end(&st, DLS_END_HOST) == MODE_OFF, "host end hands back OFF");

    printf("# D8 reason names\n");
    CHECK(strcmp(dls_end_name(DLS_END_HOST),    "host")    == 0, "host");
    CHECK(strcmp(dls_end_name(DLS_END_EXPIRED), "expired") == 0, "expired");
    CHECK(strcmp(dls_end_name(DLS_END_SLEEP),   "sleep")   == 0, "sleep");
    CHECK(strcmp(dls_end_name(DLS_END_NONE),    "")        == 0, "none");

    printf("# D9 nesting bound\n");
    CHECK(depth_ok("{\"format\":\"nc-flash-1\",\"pids\":[{\"PID\":\"010C\"}],"
                   "\"can_filters\":[{\"frame_id\":513,\"parameters\":[{\"name\":\"RPM\"}]}],"
                   "\"calculated\":[]}", DLS_JSON_MAX_DEPTH), "a real channel list");
    CHECK(depth_ok("{\"pids\":[]}", 2),  "exactly at the bound");
    CHECK(!depth_ok("{\"pids\":[[]]}", 2), "one past the bound");
    CHECK(depth_ok("{\"a\":\"[[[[[[[[\"}", 1), "brackets inside a string do not count");
    CHECK(depth_ok("{\"a\":\"\\\"[[[[\"}", 1), "nor after an escaped quote");
    CHECK(depth_ok("{\"a\":\"x\\\\\",\"b\":[]}", 2), "an escaped backslash ends the escape");
    CHECK(depth_ok("[][][][][][][][]", 1), "siblings are not depth");
    {
        char bomb[4096];
        memset(bomb, '[', sizeof(bomb));
        CHECK(!dls_json_depth_ok(bomb, sizeof(bomb), DLS_JSON_MAX_DEPTH), "a nesting bomb is refused");
    }
    CHECK(!dls_json_depth_ok(NULL, 0, DLS_JSON_MAX_DEPTH), "NULL is refused");

    if (g_failures)
    {
        printf("FAIL: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("ok: datalog_session %d checks\n", g_checks);
    return 0;
}
