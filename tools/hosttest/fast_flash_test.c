/* Host tests for the fast flash (main/fast_flash_logic.c) against a fake ECU that implements the
 * fast-flash kernel v3 protocol over a 1 MB model flash (fast_flash_fake_ecu.c).
 *
 * Test data comes from tools/hosttest/fast_flash_gen.py (which uses the reference packer
 * tools/fast_flash_package.py); run.sh generates it and passes the directory as argv[1].
 *
 * What every live case must show, whatever fault was injected:
 *   - it ends either complete and verified (flash == image outside the counter page, the commit
 *     page written once, last, onto a flash that already matched) or with FFERR and, past the point
 *     of no return, the entry pointer still erased (the ECU waits in its boot loader);
 *   - before the point of no return the flash is untouched and, if the kernel ran, 11 01 was sent;
 *   - nothing below 0x2000 is ever erased or programmed;
 *   - no two status lines are further apart than the keepalive allows.
 *
 * Build + run: tools/hosttest/run.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fast_flash_logic.h"
#include "fast_flash_fake_ecu.h"

static int g_failures = 0;
static int g_checks = 0;

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

/* ---- data ----------------------------------------------------------------------------------- */
static const char *g_dir;
static uint8_t *g_A, *g_B;

static uint8_t *load(const char *name, long *len)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    b[n] = 0;
    if (len) *len = n;
    return b;
}

/* ---- harness -------------------------------------------------------------------------------- */
typedef struct {
    fake_ecu_t *e;
    uint32_t clock;
    char log[1 << 18];
    size_t loglen;
    uint32_t last_line, max_gap;
    int lines;
    int gone_after_lines;     /* emit fails from this line on (0 = never) */
    bool gone_at_ponr;        /* emit fails once ponr is set */
    uint8_t *pkg;             /* the package of the op in flight */
    const char *corrupt_on;   /* flip a package byte when a line starting with this goes out */
    int persists;
} harness_t;

static harness_t *H;

static int op_send(void *c, const uint8_t *req, uint32_t len) { return fe_send(((harness_t *)c)->e, req, len); }
static int op_recv(void *c, uint8_t *b, uint32_t cap, uint32_t *len, uint32_t t)
{
    return fe_recv(((harness_t *)c)->e, b, cap, len, t);
}
static void op_flush(void *c) { fe_flush(((harness_t *)c)->e); }
static uint32_t op_now(void *c) { return ((harness_t *)c)->clock; }
static void op_sleep(void *c, uint32_t ms) { ((harness_t *)c)->clock += ms; }
static int op_emit(void *c, const char *line, bool ponr)
{
    harness_t *h = (harness_t *)c;
    uint32_t gap = h->clock - h->last_line;
    if (gap > h->max_gap) h->max_gap = gap;
    h->last_line = h->clock;
    h->lines++;
    size_t n = strlen(line);
    if (h->loglen + n + 1 < sizeof(h->log)) {
        memcpy(h->log + h->loglen, line, n);
        h->loglen += n;
        h->log[h->loglen] = 0;
    }
    if (h->corrupt_on && h->pkg && strncmp(line, h->corrupt_on, strlen(h->corrupt_on)) == 0) {
        h->pkg[0x2000] ^= 0x01;   /* PSRAM changed under the op */
        h->corrupt_on = NULL;
    }
    if (h->gone_after_lines && h->lines >= h->gone_after_lines) return -1;
    if (h->gone_at_ponr && ponr) return -1;
    return 0;
}
static void op_persist(void *c, const char *ev)
{
    (void)ev;
    ((harness_t *)c)->persists++;
}

static ff_ops_t ops_for(harness_t *h)
{
    ff_ops_t o = { h, op_send, op_recv, op_flush, op_now, op_sleep, op_emit, op_persist, NULL };
    return o;
}

static harness_t *harness_new(const uint8_t *initial)
{
    harness_t *h = calloc(1, sizeof(*h));
    h->e = calloc(1, sizeof(fake_ecu_t));
    h->clock = 1000;
    fe_init(h->e, initial, &h->clock);
    h->e->expect = g_B;
    h->last_line = h->clock;
    H = h;
    return h;
}

static void harness_free(harness_t *h)
{
    fe_free(h->e);
    free(h->e);
    free(h);
}

/* One op on h: returns ff_run's code; *ffo receives the op (caller frees). */
static int run(harness_t *h, char mode, const char *json_name, const char *pkg_name, bool live, ff_t **ffo)
{
    long jl = 0, pl = 0;
    char *json = (char *)load(json_name, &jl);
    uint8_t *pkg = load(pkg_name, &pl);
    ff_t *ff = calloc(1, sizeof(ff_t));
    ff_ops_t o = ops_for(h);
    ff_init(ff, &o, mode, live);
    h->pkg = pkg;
    int rc = ff_check(ff, json, (size_t)jl, pkg, (uint32_t)pl);
    if (!rc) rc = ff_run(ff);
    h->pkg = NULL;
    free(json);
    /* pkg stays referenced by ff only during ff_run */
    free(pkg);
    ff->pkg = NULL;
    *ffo = ff;
    return rc;
}

static bool flash_equals_outside_counter(const uint8_t *flash, const uint8_t *img, uint32_t *where)
{
    for (uint32_t a = 0; a < FF_ROM_SIZE; a++) {
        if (a >= FF_COUNTER_PAGE && a < FF_COUNTER_PAGE + FF_PAGE) continue;
        if (flash[a] != img[a]) {
            *where = a;
            return false;
        }
    }
    return true;
}

static bool entry_erased(const fake_ecu_t *e)
{
    return e->flash[FF_ENTRY_POINTER] == 0xFF && e->flash[FF_ENTRY_POINTER + 1] == 0xFF &&
           e->flash[FF_ENTRY_POINTER + 2] == 0xFF && e->flash[FF_ENTRY_POINTER + 3] == 0xFF;
}

/* The end-state invariants every live case must satisfy. */
static void check_end(const char *name, harness_t *h, ff_t *ff, int rc, const uint8_t *before)
{
    fake_ecu_t *e = h->e;
    uint32_t where = 0;
    CHECK(e->low_requests == 0, "%s: a request below 0x2000 reached the ECU (%d)", name, e->low_requests);
    CHECK(e->decode_starved_live == 0, "%s: fake decoder self-check", name);
    CHECK(h->max_gap <= 8000, "%s: %lu ms without a status line", name, (unsigned long)h->max_gap);
    if (rc == 0 && !ff->nothing_to_do && ff->st.mode != 'C') {
        CHECK(flash_equals_outside_counter(e->flash, g_B, &where), "%s: flash differs from the image at 0x%06lX",
              name, (unsigned long)where);
        CHECK(!entry_erased(e), "%s: complete but the entry pointer is erased", name);
        CHECK(e->commit_programmed >= 1 && e->commit_on_good_image == e->commit_programmed,
              "%s: commit page written %d times, %d onto a matching flash", name, e->commit_programmed,
              e->commit_on_good_image);
        CHECK(e->programs_after_commit == 0, "%s: %d pages programmed after the commit page", name,
              e->programs_after_commit);
        CHECK(e->resets == 1, "%s: ECU reset %d times", name, e->resets);
        CHECK(strstr(h->log, "NCFFDONE mode=") != NULL, "%s: no NCFFDONE", name);
    } else if (rc > 0 && ff->err_ponr) {
        CHECK(entry_erased(e) || rc == FF_ST_COMMIT_LINK || rc == FF_ST_COMMIT_STUCK,
              "%s: FFERR st=%d past the point of no return but the entry pointer is programmed", name, rc);
        CHECK(e->resets == 0, "%s: reset after a failure past the point of no return (kernel must stay up)", name);
        CHECK(strstr(h->log, "ponr=1") != NULL, "%s: FFERR without ponr=1", name);
    } else if (rc > 0) {
        CHECK(memcmp(e->flash, before, FF_ROM_SIZE) == 0, "%s: FFERR st=%d before the point of no return but the "
              "flash changed", name, rc);
        CHECK(e->erase_count[15] == 0, "%s: block 15 erased before the point of no return", name);
        CHECK(strstr(h->log, "ponr=0") != NULL, "%s: FFERR without ponr=0", name);
    }
}

static int erased_mask(const fake_ecu_t *e)
{
    int m = 0;
    for (int b = 0; b < 16; b++)
        if (e->erase_count[b]) m |= 1 << b;
    return m;
}

#define MASK(...) mask_of((int[]){ __VA_ARGS__, -1 })
static int mask_of(const int *b)
{
    int m = 0;
    for (; *b >= 0; b++) m |= 1 << *b;
    return m;
}

/* A plain live run from A to B with optional faults; checks the end state. */
typedef void (*setup_fn)(harness_t *);
static int live_case(const char *name, setup_fn setup, ff_t **ffo, harness_t **ho)
{
    harness_t *h = harness_new(g_A);
    if (setup) setup(h);
    ff_t *ff;
    int rc = run(h, 'L', "b.json", "b.ffp", true, &ff);
    check_end(name, h, ff, rc, g_A);
    *ffo = ff;
    *ho = h;
    return rc;
}

static void done_case(ff_t *ff, harness_t *h)
{
    free(ff);
    harness_free(h);
}

/* ---- the cases ------------------------------------------------------------------------------ */

static void t_dry_run(void)
{
    printf("# T1 dry run: package checked, nothing on CAN\n");
    harness_t *h = harness_new(g_A);
    ff_t *ff;
    int rc = run(h, 'D', "b.json", "b.ffp", false, &ff);
    CHECK(rc == 0, "dry run rc=%d", rc);
    CHECK(h->e->requests == 0, "dry run sent %d requests", h->e->requests);
    CHECK(strstr(h->log, "NCFFDONE mode=D") != NULL, "no NCFFDONE mode=D");
    done_case(ff, h);
}

static void t_live_gate(void)
{
    printf("# T2 compile gate: L and R refused without NCFF_ALLOW_LIVE, nothing on CAN\n");
    for (int i = 0; i < 2; i++) {
        harness_t *h = harness_new(g_A);
        ff_t *ff;
        int rc = run(h, i ? 'R' : 'L', "b.json", "b.ffp", false, &ff);
        CHECK(rc == FF_ST_LIVE_GATE, "mode %c rc=%d", i ? 'R' : 'L', rc);
        CHECK(h->e->requests == 0, "gate sent %d requests", h->e->requests);
        CHECK(strstr(h->log, "FFERR a=000000 st=7") != NULL, "no FFERR st=7");
        done_case(ff, h);
    }
}

static void t_compare(void)
{
    printf("# T3 compare only: CRC table, no erase, ECU reset\n");
    harness_t *h = harness_new(g_A);
    ff_t *ff;
    int rc = run(h, 'C', "b.json", "b.ffp", false, &ff);
    CHECK(rc == 0, "compare rc=%d", rc);
    CHECK(ff->diff_mask == MASK(3, 9, 15), "diff mask 0x%04X", ff->diff_mask);
    CHECK(erased_mask(h->e) == 0, "compare erased 0x%04X", erased_mask(h->e));
    CHECK(memcmp(h->e->flash, g_A, FF_ROM_SIZE) == 0, "compare changed the flash");
    CHECK(h->e->resets == 1, "compare: %d resets", h->e->resets);
    CHECK(strstr(h->log, "NCFFCMP diff=3,9,15 boot=1 cut=0 rewrite=-") != NULL, "NCFFCMP line: %s", h->log);
    CHECK(h->e->counts[FK_ERASE] == 0 && h->e->counts[FK_OPEN] == 0 && h->e->counts[FK_OPENLZ] == 0,
          "compare sent erase/program requests");
    done_case(ff, h);
}

static void t_live(void)
{
    printf("# T4 live: only differing blocks + 15 rewritten, verified, committed last\n");
    ff_t *ff;
    harness_t *h;
    int rc = live_case("live", NULL, &ff, &h);
    CHECK(rc == 0, "live rc=%d (%s)", rc, h->log);
    CHECK(erased_mask(h->e) == MASK(3, 9, 15), "erased 0x%04X", erased_mask(h->e));
    for (int b = 0; b < 16; b++) CHECK(h->e->erase_count[b] <= 1, "block %d erased %d times", b, h->e->erase_count[b]);
    CHECK(ff->rewrite_mask == MASK(3, 9, 15), "rewritten 0x%04X", ff->rewrite_mask);
    CHECK(h->persists == 1, "journal written %d times", h->persists);
    CHECK(h->e->probes_before_first_erase == (int)FF_PROBE_COUNT, "%d link probes before the first erase",
          h->e->probes_before_first_erase);
    CHECK(strstr(h->log, "NCFFPROBE ok=1 lost=0") != NULL, "NCFFPROBE line");
    CHECK(strstr(h->log, "NCFFCMP diff=3,9,15 boot=1 cut=0 rewrite=3,9,15") != NULL, "NCFFCMP");
    CHECK(strstr(h->log, "NCFFKERN v=3 cls=1 ready=1 resumed=0") != NULL, "NCFFKERN");
    CHECK(strstr(h->log, "NCFFRES {") != NULL, "NCFFRES");
    CHECK(ff->resends == 0 && ff->reerases == 0, "clean run had resends %lu reerases %lu",
          (unsigned long)ff->resends, (unsigned long)ff->reerases);
    /* the counter page holds the kernel's counter, never image data */
    CHECK(memcmp(h->e->flash + FF_COUNTER_PAGE, "\x00\x01\xff\xfe_WDS", 8) == 0, "counter page not rewritten");
    done_case(ff, h);
}

static void t_nothing(void)
{
    printf("# T5 same image: nothing to do, nothing erased\n");
    harness_t *h = harness_new(g_A);
    h->e->expect = g_A;
    ff_t *ff;
    int rc = run(h, 'L', "a.json", "a.ffp", true, &ff);
    CHECK(rc == 0 && ff->nothing_to_do, "rc=%d nothing=%d", rc, ff->nothing_to_do);
    CHECK(erased_mask(h->e) == 0, "erased 0x%04X", erased_mask(h->e));
    CHECK(h->e->resets == 1, "resets %d", h->e->resets);
    CHECK(strstr(h->log, "NCFFDONE mode=L nothing=1") != NULL, "NCFFDONE nothing=1");
    CHECK(h->persists == 0, "journal written without a point of no return");
    done_case(ff, h);
}

static void t_rewrite_all(void)
{
    printf("# T6 rewrite_all: every block 2..15 once\n");
    harness_t *h = harness_new(g_A);
    ff_t *ff;
    int rc = run(h, 'L', "b_all.json", "b.ffp", true, &ff);
    check_end("rewrite_all", h, ff, rc, g_A);
    CHECK(rc == 0, "rc=%d", rc);
    CHECK(erased_mask(h->e) == 0xFFFC, "erased 0x%04X", erased_mask(h->e));
    done_case(ff, h);
}

static void t_cut_short(void)
{
    printf("# T7 earlier flash cut short (entry pointer erased): every block rewritten\n");
    uint8_t *cut = malloc(FF_ROM_SIZE);
    memcpy(cut, g_A, FF_ROM_SIZE);
    memset(cut + 0xE0000, 0xFF, 0x20000);
    memcpy(cut + FF_COUNTER_PAGE, "\x00\x01\xff\xfe_WDS", 8);
    harness_t *h = harness_new(cut);
    ff_t *ff;
    int rc = run(h, 'L', "b.json", "b.ffp", true, &ff);
    check_end("cut_short", h, ff, rc, cut);
    CHECK(rc == 0 && ff->cut_short, "rc=%d cut=%d", rc, ff->cut_short);
    CHECK(erased_mask(h->e) == 0xFFFC, "erased 0x%04X", erased_mask(h->e));
    done_case(ff, h);
    free(cut);
}

static void t_boot(void)
{
    printf("# T8 boot block differs: refused before any erase unless the manifest allows it\n");
    harness_t *h = harness_new(g_A);
    ff_t *ff;
    int rc = run(h, 'L', "b_boot.json", "b.ffp", true, &ff);
    check_end("boot_refuse", h, ff, rc, g_A);
    CHECK(rc == FF_ST_BOOT, "rc=%d", rc);
    CHECK(h->e->resets == 1, "ECU not reset after the refusal (%d)", h->e->resets);
    done_case(ff, h);

    h = harness_new(g_A);
    rc = run(h, 'L', "b_bootallow.json", "b.ffp", true, &ff);
    check_end("boot_allow", h, ff, rc, g_A);
    CHECK(rc == 0 && !ff->boot_same, "rc=%d boot_same=%d", rc, ff->boot_same);
    done_case(ff, h);
}

static void t_manifests(void)
{
    printf("# T9 every broken manifest is refused with no CAN traffic\n");
    long n = 0;
    char *cases = (char *)load("cases.txt", &n);
    int count = 0;
    for (char *line = cases, *nl; line && *line; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char jn[128], pn[128];
        int stage = 0;
        if (sscanf(line, "%127s %127s %d", jn, pn, &stage) != 3) continue;
        harness_t *h = harness_new(g_A);
        ff_t *ff;
        int rc = run(h, 'L', jn, pn, true, &ff);
        CHECK(rc == stage, "%s: rc=%d, expected %d", jn, rc, stage);
        CHECK(h->e->requests == 0, "%s: %d requests sent", jn, h->e->requests);
        char want[32];
        snprintf(want, sizeof(want), "st=%d ", stage);
        CHECK(strstr(h->log, want) != NULL, "%s: no FFERR %s", jn, want);
        done_case(ff, h);
        count++;
    }
    CHECK(count >= 20, "only %d manifest cases ran", count);
    free(cases);
}

static void s_kcrc(harness_t *h) { fe_add_fault(h->e, FK_UPLOAD, 2, FA_CORRUPT, 0, 0); }
static void s_kver(harness_t *h) { h->e->kver = 2; }
static void s_ready(harness_t *h) { h->e->kready = 0; }

static void t_kernel_checks(void)
{
    printf("# T10 kernel checks before the first erase: self CRC, version, ready\n");
    struct { const char *n; setup_fn s; int st; } c[] = {
        { "kernel_crc", s_kcrc, FF_ST_KCRC },
        { "kernel_version", s_kver, FF_ST_KVERSION },
        { "kernel_not_ready", s_ready, FF_ST_NOT_READY },
    };
    for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        ff_t *ff;
        harness_t *h;
        int rc = live_case(c[i].n, c[i].s, &ff, &h);
        CHECK(rc == c[i].st, "%s: rc=%d", c[i].n, rc);
        CHECK(h->e->resets == 1, "%s: ECU not sent back to its application (%d)", c[i].n, h->e->resets);
        CHECK(h->e->counts[FK_ERASE] == 0, "%s: erase sent", c[i].n);
        done_case(ff, h);
    }
}

static void s_up_lost(harness_t *h) { fe_add_fault(h->e, FK_UPLOAD, 3, FA_DROP_ANSWER, 0, 0); }
static void s_up_lost_last(harness_t *h) { fe_add_fault(h->e, FK_UPLOAD, 6, FA_DROP_ANSWER, 0, 0); }
static void s_up_norestart(harness_t *h)
{
    fe_add_fault(h->e, FK_UPLOAD, 3, FA_DROP_ANSWER, 0, 0);
    h->e->loader_restart_ok = false;
}
static void s_up_dropreq(harness_t *h) { fe_add_fault(h->e, FK_UPLOAD, 2, FA_DROP_REQUEST, 0, 0); }

static void t_upload(void)
{
    printf("# T11 kernel upload: lost answer + 0x22 on resend, last block, no restart, lost block\n");
    ff_t *ff;
    harness_t *h;
    int rc = live_case("upload_lost", s_up_lost, &ff, &h);
    CHECK(rc == 0 && ff->restarts == 1, "rc=%d restarts=%lu", rc, (unsigned long)ff->restarts);
    CHECK(h->e->counts[FK_DL] == 2, "RequestDownload sent %d times", h->e->counts[FK_DL]);
    done_case(ff, h);

    rc = live_case("upload_lost_last", s_up_lost_last, &ff, &h);
    CHECK(rc == 0 && ff->restarts == 0, "rc=%d restarts=%lu", rc, (unsigned long)ff->restarts);
    CHECK(h->e->counts[FK_DL] == 1, "RequestDownload sent %d times", h->e->counts[FK_DL]);
    done_case(ff, h);

    rc = live_case("upload_no_restart", s_up_norestart, &ff, &h);
    CHECK(rc == FF_ST_UPLOAD_LOST, "rc=%d", rc);
    CHECK(h->e->resets == 0, "reset sent although no kernel runs");
    done_case(ff, h);

    rc = live_case("upload_request_lost", s_up_dropreq, &ff, &h);
    CHECK(rc == 0 && ff->resends == 1, "rc=%d resends=%lu", rc, (unsigned long)ff->resends);
    done_case(ff, h);
}

static void s_piece_lost_answer(harness_t *h) { fe_add_fault(h->e, FK_PIECE, 40, FA_DROP_ANSWER, 0, 0); }
static void s_piece_lost_req(harness_t *h) { fe_add_fault(h->e, FK_PIECE, 41, FA_DROP_REQUEST, 0, 0); }
static void s_piece_fc(harness_t *h) { fe_add_fault(h->e, FK_PIECE, 12, FA_FC_FAIL, 0, 0); }
static void s_piece_refused(harness_t *h) { fe_add_fault(h->e, FK_PIECE, 45, FA_REFUSE, 0, 0); }
static void s_piece_corrupt(harness_t *h) { fe_add_fault(h->e, FK_PIECE, 30, FA_CORRUPT, 0, 0); }
static void s_piece_late(harness_t *h) { fe_add_fault(h->e, FK_PIECE, 20, FA_LATE, 7000, 0); }
static void s_crc_late(harness_t *h) { fe_add_fault(h->e, FK_CRC, 5, FA_LATE, 11000, 0); }
static void s_noise(harness_t *h) { h->e->noise = 1; }
static void s_slow_erase(harness_t *h) { fe_add_fault(h->e, FK_ERASE, 2, FA_SLOW, 23000, 0); }
static void s_slow_piece(harness_t *h) { fe_add_fault(h->e, FK_PIECE, 10, FA_SLOW, 17000, 0); }
static void s_close_lost(harness_t *h) { fe_add_fault(h->e, FK_CLOSE, 2, FA_DROP_ANSWER, 0, 0); }
static void s_erase_lost(harness_t *h) { fe_add_fault(h->e, FK_ERASE, 1, FA_DROP_ANSWER, 0, 0); }
static void s_open_lost(harness_t *h) { fe_add_fault(h->e, FK_OPENLZ, 2, FA_DROP_ANSWER, 0, 0); }
static void s_pc_gone_ponr(harness_t *h) { h->gone_at_ponr = true; }

static void t_link_faults(void)
{
    printf("# T12 faults past the point of no return that the flash rides out\n");
    struct { const char *n; setup_fn s; } c[] = {
        { "piece_answer_lost", s_piece_lost_answer }, { "piece_request_lost", s_piece_lost_req },
        { "piece_no_flow_control", s_piece_fc },      { "piece_refused_once", s_piece_refused },
        { "piece_answer_late", s_piece_late },
        { "crc_answer_late", s_crc_late },            { "foreign_noise", s_noise },
        { "slow_erase", s_slow_erase },               { "slow_piece", s_slow_piece },
        { "close_answer_lost", s_close_lost },        { "erase_answer_lost", s_erase_lost },
        { "open_answer_lost", s_open_lost },          { "pc_gone_after_ponr", s_pc_gone_ponr },
    };
    for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        ff_t *ff;
        harness_t *h;
        int rc = live_case(c[i].n, c[i].s, &ff, &h);
        CHECK(rc == 0, "%s: rc=%d err st=%d nrc=%02X blk=%d\n%s", c[i].n, rc, ff->err_stage, ff->err_nrc,
              ff->err_blk, h->log);
        if (!strcmp(c[i].n, "piece_refused_once"))
            CHECK(ff->reerases == 1, "%s: reerases=%lu", c[i].n, (unsigned long)ff->reerases);
        if (!strcmp(c[i].n, "foreign_noise")) CHECK(ff->foreign > 50, "foreign=%lu", (unsigned long)ff->foreign);
        if (!strcmp(c[i].n, "slow_erase") || !strcmp(c[i].n, "slow_piece"))
            CHECK(ff->pendings > 3, "%s: pendings=%lu", c[i].n, (unsigned long)ff->pendings);
        done_case(ff, h);
    }
}

static void s_refused_twice(harness_t *h) { fe_add_fault(h->e, FK_PIECE, 45, FA_REFUSE, 0, 1); }
/* The point of no return comes ~1.7 s into a run (clock starts at 1000 ms); 4000 ms falls in the
   write phase. 16 min outlasts every retry budget (4 quick retries + 5 min); 60 s does not. */
static void s_outage_long(harness_t *h)
{
    h->e->outage_from = 4000;
    h->e->outage_to = 4000 + 1000000;
}
static void s_outage_short(harness_t *h)
{
    h->e->outage_from = 4000;
    h->e->outage_to = 4000 + 60000;
}
static void s_pc_gone_early(harness_t *h) { h->gone_after_lines = 4; }

static void t_fail_safe(void)
{
    printf("# T13 faults that end in FFERR: the ECU is left waiting in its boot loader, or untouched\n");
    ff_t *ff;
    harness_t *h;
    int rc = live_case("refused_twice", s_refused_twice, &ff, &h);
    CHECK(rc == FF_ST_PIECE && ff->err_ponr, "rc=%d ponr=%d", rc, ff->err_ponr);
    done_case(ff, h);

    /* A piece corrupted on the way in (CAN's own CRC makes this artificial): the kernel either
       refuses it (re-erase, resend, complete) or decodes wrong data that the verify catches. */
    rc = live_case("piece_corrupted_once", s_piece_corrupt, &ff, &h);
    CHECK(rc == 0 || (rc > 0 && ff->err_ponr && h->e->commit_programmed == 0), "rc=%d ponr=%d", rc, ff->err_ponr);
    done_case(ff, h);

    rc = live_case("outage_short", s_outage_short, &ff, &h);
    CHECK(rc == 0 && h->clock > 64000 && h->e->erase_count[9] >= 2, "rc=%d clock=%lu erases of 9: %d", rc,
          (unsigned long)h->clock, h->e->erase_count[9]);
    done_case(ff, h);

    rc = live_case("outage_over_5_min", s_outage_long, &ff, &h);
    CHECK(rc >= FF_ST_ERASE && ff->err_ponr, "rc=%d ponr=%d", rc, ff->err_ponr);
    done_case(ff, h);

    rc = live_case("pc_gone_before_ponr", s_pc_gone_early, &ff, &h);
    CHECK(rc == FF_ST_HOST_GONE && !ff->err_ponr, "rc=%d ponr=%d", rc, ff->err_ponr);
    CHECK(h->e->resets == 1, "ECU not reset after the PC left (%d)", h->e->resets);
    done_case(ff, h);

    printf("# T14 a package whose block-9 stream is wrong (packer bug): never committed\n");
    h = harness_new(g_A);
    rc = run(h, 'L', "b_corrupt.json", "b_corrupt.ffp", true, &ff);
    check_end("corrupt_stream", h, ff, rc, g_A);
    CHECK(rc > 0 && ff->err_ponr && h->e->commit_programmed == 0, "rc=%d ponr=%d commits=%d", rc, ff->err_ponr,
          h->e->commit_programmed);
    done_case(ff, h);
}

static void s_commit_lost(harness_t *h) { fe_add_fault(h->e, FK_PLAIN, 1, FA_DROP_ANSWER, 0, 0); }
static void s_commit_bad(harness_t *h) { fe_add_fault(h->e, FK_PLAIN, 1, FA_REFUSE, 0, 0); }
/* closes: block 3, block 9, block 15 (two ranges), then the commit page = the 5th */
static void s_commit_close_lost(harness_t *h) { fe_add_fault(h->e, FK_CLOSE, 5, FA_DROP_ANSWER, 0, 0); }

static void t_commit(void)
{
    printf("# T15 commit: a lost answer never erases a good flash; a bad page erases block 15 again\n");
    ff_t *ff;
    harness_t *h;
    int rc = live_case("commit_answer_lost", s_commit_lost, &ff, &h);
    CHECK(rc == 0, "rc=%d\n%s", rc, h->log);
    CHECK(h->e->erase_count[15] == 1, "block 15 erased %d times", h->e->erase_count[15]);
    done_case(ff, h);

    rc = live_case("commit_close_lost", s_commit_close_lost, &ff, &h);
    CHECK(rc == 0, "rc=%d", rc);
    CHECK(h->e->erase_count[15] == 1, "block 15 erased %d times", h->e->erase_count[15]);
    done_case(ff, h);

    rc = live_case("commit_bad_page", s_commit_bad, &ff, &h);
    CHECK(rc == FF_ST_COMMIT && ff->err_ponr, "rc=%d", rc);
    CHECK(h->e->erase_count[15] == 2, "block 15 erased %d times", h->e->erase_count[15]);
    done_case(ff, h);
}

static void s_halt_mid(harness_t *h) { fe_add_fault(h->e, FK_PIECE, 60, FA_HALT, 0, 0); }

static void t_resume(void)
{
    printf("# T16 dongle loses power past the point of no return; mode R finishes the job\n");
    harness_t *h = harness_new(g_A);
    s_halt_mid(h);
    ff_t *ff;
    int rc = run(h, 'L', "b.json", "b.ffp", true, &ff);
    CHECK(rc == -1 && ff->halted, "halt rc=%d", rc);
    CHECK(entry_erased(h->e), "entry pointer programmed after a halt mid-write");
    free(ff);
    h->e->nfaults = 0;
    h->lines = 0;
    h->last_line = h->clock;
    h->loglen = 0;
    h->log[0] = 0;
    /* a resume that fails before writing must keep the kernel up and say ponr=1 */
    h->e->kver = 2;
    rc = run(h, 'R', "b.json", "b.ffp", true, &ff);
    check_end("resume_wrong_version", h, ff, rc, g_A);
    CHECK(rc == FF_ST_KVERSION && ff->err_ponr && h->e->resets == 0, "rc=%d ponr=%d resets=%d", rc, ff->err_ponr,
          h->e->resets);
    free(ff);
    h->e->kver = 3;
    h->loglen = 0;
    h->log[0] = 0;
    rc = run(h, 'R', "b.json", "b.ffp", true, &ff);
    check_end("resume", h, ff, rc, g_A);
    CHECK(rc == 0 && ff->resumed && ff->cut_short, "rc=%d resumed=%d cut=%d", rc, ff->resumed, ff->cut_short);
    CHECK(h->e->counts[FK_DL] == 1, "kernel uploaded again on resume");
    CHECK(h->e->erase_count[3] == 2 && h->e->erase_count[2] == 1, "resume did not rewrite every block");
    done_case(ff, h);

    printf("# T17 resume with no kernel running: FFERR 23, nothing touched, no reset\n");
    h = harness_new(g_A);
    h->e->app_running = true;
    rc = run(h, 'R', "b.json", "b.ffp", true, &ff);
    check_end("resume_no_kernel", h, ff, rc, g_A);
    CHECK(rc == FF_ST_IDENTIFY && h->e->resets == 0, "rc=%d resets=%d", rc, h->e->resets);
    CHECK(strstr(h->log, "ponr=0 app=u") != NULL, "R without a kernel must say the ECU state is unknown: %s", h->log);
    done_case(ff, h);
}

static void s_probe_lost(harness_t *h) { fe_add_fault(h->e, FK_PROBE, 1, FA_DROP_ANSWER, 0, 1); }
static void s_counter100(harness_t *h) { h->e->force_counter = 100; }
static void s_commit_stuck(harness_t *h)
{
    fe_add_fault(h->e, FK_PLAIN, 1, FA_CORRUPT, 0, 0);          /* a wrong commit page goes in */
    fe_add_fault(h->e, FK_ERASE, 4, FA_DROP_REQUEST, 0, 1);     /* and block 15 cannot be erased again */
}
static void s_commit_lie72(harness_t *h) { fe_add_fault(h->e, FK_PLAIN, 1, FA_LIE72, 0, 0); }
static void s_commit_crc_lost(harness_t *h) { fe_add_fault(h->e, FK_CRCC, 1, FA_DROP_REQUEST, 0, 1); }
static void s_dup_piece(harness_t *h) { fe_add_fault(h->e, FK_PIECE, 20, FA_DUP, 3, 0); }
static void s_psram(harness_t *h) { h->corrupt_on = "NCFFCMP"; }

static void t_review_2026_11_16(void)
{
    printf("# T18 review 2026-11-16 fixes\n");
    ff_t *ff;
    harness_t *h;

    /* finding 1: the 1 KB probe fails -> nothing erased, ECU back to its application */
    int rc = live_case("probe_lost", s_probe_lost, &ff, &h);
    CHECK(rc == FF_ST_PROBE && !ff->err_ponr && h->e->resets == 1 && h->e->counts[FK_ERASE] == 0,
          "rc=%d ponr=%d resets=%d erases=%d", rc, ff->err_ponr, h->e->resets, h->e->counts[FK_ERASE]);
    done_case(ff, h);
    h = harness_new(g_A);
    s_probe_lost(h);
    rc = run(h, 'C', "b.json", "b.ffp", false, &ff);
    check_end("probe_lost_compare", h, ff, rc, g_A);
    CHECK(rc == FF_ST_PROBE && h->e->resets == 1, "compare: rc=%d resets=%d", rc, h->e->resets);
    done_case(ff, h);

    /* finding 3: the first block-15 erase refused outright -> ponr=0, 11 01 */
    rc = live_case("erase15_refused", s_counter100, &ff, &h);
    CHECK(rc == FF_ST_ERASE15_REFUSED && !ff->err_ponr && h->e->resets == 1, "rc=%d ponr=%d resets=%d", rc,
          ff->err_ponr, h->e->resets);
    CHECK(strstr(h->log, "ponr=0 app=1") != NULL, "erase15_refused FFERR: %s", h->log);
    done_case(ff, h);

    /* finding 4: wrong commit page and the re-erase fails -> its own stage, kernel left up */
    rc = live_case("commit_stuck", s_commit_stuck, &ff, &h);
    CHECK(rc == FF_ST_COMMIT_STUCK && ff->err_ponr && h->e->resets == 0, "rc=%d ponr=%d resets=%d", rc, ff->err_ponr,
          h->e->resets);
    CHECK(strstr(h->log, "commit_reerase_failed") != NULL && strstr(h->log, "app=u") != NULL, "commit_stuck lines");
    done_case(ff, h);

    /* finding 7: "72" on the commit page is not trusted even when the CRC matches */
    rc = live_case("commit_72_crc_ok", s_commit_lie72, &ff, &h);
    CHECK(rc == FF_ST_COMMIT && h->e->erase_count[15] == 2, "rc=%d erases of 15: %d", rc, h->e->erase_count[15]);
    done_case(ff, h);

    /* st 61: the commit page went in but its CRC cannot be read */
    rc = live_case("commit_crc_lost", s_commit_crc_lost, &ff, &h);
    {
        uint32_t where = 0;
        CHECK(rc == FF_ST_COMMIT_LINK && h->e->resets == 0, "rc=%d resets=%d", rc, h->e->resets);
        CHECK(flash_equals_outside_counter(h->e->flash, g_B, &where), "st 61 but the flash is not the image");
    }
    done_case(ff, h);

    /* finding 6: a duplicate answer to the previous piece is not taken for the next one */
    rc = live_case("duplicate_piece_answer", s_dup_piece, &ff, &h);
    CHECK(rc == 0 && ff->foreign >= 1, "rc=%d foreign=%lu", rc, (unsigned long)ff->foreign);
    done_case(ff, h);

    /* the package changes in RAM between the checks and the first erase */
    rc = live_case("package_changed_in_ram", s_psram, &ff, &h);
    CHECK(rc == FF_ST_CRC && !ff->err_ponr && h->e->resets == 1, "rc=%d ponr=%d resets=%d", rc, ff->err_ponr,
          h->e->resets);
    done_case(ff, h);

    /* finding 2: L while our kernel already runs (an earlier op was cut) -> "use R", kernel kept */
    uint8_t *cut = malloc(FF_ROM_SIZE);
    memcpy(cut, g_A, FF_ROM_SIZE);
    memset(cut + 0xE0000, 0xFF, 0x20000);
    memcpy(cut + FF_COUNTER_PAGE, "\x00\x01\xff\xfe_WDS", 8);
    h = harness_new(cut);
    fe_start_kernel(h->e);
    rc = run(h, 'L', "b.json", "b.ffp", true, &ff);
    check_end("l_while_kernel_runs", h, ff, rc, cut);
    CHECK(rc == FF_ST_KERNEL_RUNNING && ff->err_ponr && h->e->resets == 0, "rc=%d ponr=%d resets=%d", rc,
          ff->err_ponr, h->e->resets);
    CHECK(strstr(h->log, "app=u") != NULL, "kernel running: app=u");
    done_case(ff, h);

    /* finding 2: an L that fails before its own point of no return on an ECU with no application
       keeps the kernel (no 11 01), says app=0 and journals it */
    h = harness_new(cut);
    rc = run(h, 'L', "b_boot.json", "b.ffp", true, &ff);
    check_end("no_app_boot_refused", h, ff, rc, cut);
    CHECK(rc == FF_ST_BOOT && !ff->err_ponr && h->e->resets == 0 && h->persists >= 1, "rc=%d ponr=%d resets=%d "
          "persists=%d", rc, ff->err_ponr, h->e->resets, h->persists);
    CHECK(strstr(h->log, "ponr=0 app=0") != NULL, "no_app FFERR: %s", h->log);
    done_case(ff, h);
    free(cut);
}

static void t_crc32(void)
{
    printf("# T0 CRC-32 matches zlib\n");
    CHECK(ff_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u, "check value");
    const uint8_t ff4[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(ff_crc32(0, ff4, 4) == 0xFFFFFFFFu, "crc32(FF*4)");
    CHECK(ff_crc32(ff_crc32(0, (const uint8_t *)"1234", 4), (const uint8_t *)"56789", 5) == 0xCBF43926u, "chained");
    CHECK(ff_crc32_combine(ff_crc32(0, (const uint8_t *)"1234", 4), ff_crc32(0, (const uint8_t *)"56789", 5), 5) ==
              0xCBF43926u, "combine");
    CHECK(ff_crc32_combine(0xCBF43926u, 0, 0) == 0xCBF43926u, "combine with nothing");
    {
        uint32_t c1 = ff_crc32(0, g_B + 0x2000, 0x7000), c2 = ff_crc32(0, g_B + 0x9000, 0xF6B00);
        CHECK(ff_crc32_combine(c1, c2, 0xF6B00) == ff_crc32(0, g_B + 0x2000, 0xFDB00), "combine over the ROM");
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: fast_flash_test <data dir from fast_flash_gen.py>\n");
        return 2;
    }
    g_dir = argv[1];
    long n = 0;
    g_A = load("A.bin", &n);
    g_B = load("B.bin", &n);

    t_crc32();
    t_dry_run();
    t_live_gate();
    t_compare();
    t_live();
    t_nothing();
    t_rewrite_all();
    t_cut_short();
    t_boot();
    t_manifests();
    t_kernel_checks();
    t_upload();
    t_link_faults();
    t_fail_safe();
    t_commit();
    t_resume();
    t_review_2026_11_16();

    printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "ok", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
