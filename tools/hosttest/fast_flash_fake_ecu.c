/* Fake NC ECU (ROM loader + fast-flash kernel v3) for tools/hosttest/fast_flash_test.c.
 * See fast_flash_fake_ecu.h. Kernel behaviour is cited as kernel.c <function>. */
#include "fast_flash_fake_ecu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fast_flash_logic.h"   /* ff_crc32, ff_block_start */

#define FE_APP_START     0x2000u
#define FE_COUNTER_PAGE  0xFFB00u
#define FE_COMMIT_PAGE   0xFFF80u
#define FE_ENTRY         0xFFFFCu
#define FE_PAGE          128u
#define FE_PENDING_MS    4000u    /* the kernel's PENDING_TICKS, about 4 s */

/* ================================================================================================ */
/* LZMA decoder, after kernel.c lz_decode()                                                          */
/* ================================================================================================ */

#define LZ_IS_MATCH   0
#define LZ_IS_REP     48
#define LZ_IS_REP_G0  60
#define LZ_IS_REP_G1  72
#define LZ_IS_REP_G2  84
#define LZ_IS_REP0L   96
#define LZ_POS_SLOT   144
#define LZ_POS_DEC    400
#define LZ_ALIGN      515
#define LZ_LEN        531
#define LZ_REP_LEN    853
#define LZ_LIT        1175
#define LZ_NPROBS     4247
#define LZ_SAFE_INPUT 64          /* one symbol never reads more than ~48 bytes */

struct fe_lzd {
    uint16_t P[LZ_NPROBS];
    uint32_t range, code, state, rep0, rep1, rep2, rep3;
    uint32_t out_pos, out_size, lpmask, pbmask;
    int started, err, starved;
    const uint8_t *in;
    uint32_t in_len, in_pos;
    uint8_t *out;
};

static uint32_t lz_in(fe_lzd_t *d)
{
    if (d->err) return 0;
    if (d->in_pos >= d->in_len) {
        d->starved = 1;
        return 0;
    }
    return d->in[d->in_pos++];
}

static void rc_norm(fe_lzd_t *d)
{
    if (d->range < 0x01000000u) {
        d->range <<= 8;
        d->code = (d->code << 8) | lz_in(d);
    }
}

static uint32_t rc_bit(fe_lzd_t *d, uint16_t *p)
{
    uint32_t v = *p, bound = (d->range >> 11) * v, b;
    if (d->code < bound) {
        d->range = bound;
        *p = (uint16_t)(v + ((2048u - v) >> 5));
        b = 0;
    } else {
        d->range -= bound;
        d->code -= bound;
        *p = (uint16_t)(v - (v >> 5));
        b = 1;
    }
    rc_norm(d);
    return b;
}

static uint32_t rc_direct(fe_lzd_t *d, uint32_t n)
{
    uint32_t res = 0, t;
    do {
        d->range >>= 1;
        d->code -= d->range;
        t = 0 - (d->code >> 31);
        d->code += d->range & t;
        if (d->code == d->range) d->err = 0x72;
        rc_norm(d);
        res = (res << 1) + t + 1;
    } while (--n);
    return res;
}

static uint32_t bittree(fe_lzd_t *d, uint16_t *p, uint32_t bits)
{
    uint32_t m = 1, top = 1;
    do {
        m = (m << 1) + rc_bit(d, p + m);
        top <<= 1;
    } while (--bits);
    return m - top;
}

static uint32_t bittree_rev(fe_lzd_t *d, uint16_t *p, uint32_t bits)
{
    uint32_t m = 1, sym = 0, mask = 1, b;
    do {
        b = rc_bit(d, p + m);
        m = (m << 1) + b;
        if (b) sym |= mask;
        mask <<= 1;
    } while (--bits);
    return sym;
}

static uint32_t lz_len(fe_lzd_t *d, uint16_t *p, uint32_t ps)
{
    if (!rc_bit(d, p)) return bittree(d, p + 2 + ps * 8, 3);
    if (!rc_bit(d, p + 1)) return 8 + bittree(d, p + 34 + ps * 8, 3);
    return 16 + bittree(d, p + 66, 8);
}

static uint32_t lz_dist(fe_lzd_t *d, uint32_t len)
{
    uint16_t *P = d->P;
    uint32_t slot = bittree(d, P + LZ_POS_SLOT + (len > 3 ? 3 : len) * 64, 6);
    if (slot < 4) return slot;
    uint32_t nb = (slot >> 1) - 1;
    uint32_t dd = (2 | (slot & 1)) << nb;
    if (slot < 14) return dd + bittree_rev(d, P + LZ_POS_DEC + dd - slot, nb);
    dd += rc_direct(d, nb - 4) << 4;
    return dd + bittree_rev(d, P + LZ_ALIGN, 4);
}

static uint32_t get_byte(fe_lzd_t *d, uint32_t dist)
{
    if (dist >= d->out_pos) { /* cannot happen after the kernel's checks; a fake bug if it does */
        d->err = 0x72;
        return 0;
    }
    return d->out[d->out_pos - dist - 1];
}

static void put_byte(fe_lzd_t *d, uint32_t b)
{
    if (d->err) return;
    if (d->out_pos >= d->out_size) {
        d->err = 0x72;
        return;
    }
    d->out[d->out_pos++] = (uint8_t)b;
}

/* One symbol (literal or match), exactly the body of the kernel's decode loop. */
static void lz_step(fe_lzd_t *d)
{
    uint16_t *P = d->P, *lit;
    uint32_t ps = d->out_pos & d->pbmask, sym, mb, mbit, bit, len, dd;
    uint32_t state = d->state;
    if (!rc_bit(d, P + LZ_IS_MATCH + state * 4 + ps)) {
        lit = P + LZ_LIT + (d->out_pos & d->lpmask) * 0x300;
        sym = 1;
        if (state >= 7) {
            mb = get_byte(d, d->rep0);
            do {
                mbit = (mb >> 7) & 1;
                mb <<= 1;
                bit = rc_bit(d, lit + (1 + mbit) * 0x100 + sym);
                sym = (sym << 1) | bit;
                if (mbit != bit) break;
            } while (sym < 0x100);
        }
        while (sym < 0x100) sym = (sym << 1) | rc_bit(d, lit + sym);
        put_byte(d, sym - 0x100);
        d->state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
        return;
    }
    if (rc_bit(d, P + LZ_IS_REP + state)) {
        if (d->out_pos == 0) {
            d->err = 0x72;
            return;
        }
        if (!rc_bit(d, P + LZ_IS_REP_G0 + state)) {
            if (!rc_bit(d, P + LZ_IS_REP0L + state * 4 + ps)) {
                d->state = state < 7 ? 9 : 11;
                put_byte(d, get_byte(d, d->rep0));
                return;
            }
        } else {
            if (!rc_bit(d, P + LZ_IS_REP_G1 + state)) {
                dd = d->rep1;
            } else {
                if (!rc_bit(d, P + LZ_IS_REP_G2 + state)) {
                    dd = d->rep2;
                } else {
                    dd = d->rep3;
                    d->rep3 = d->rep2;
                }
                d->rep2 = d->rep1;
            }
            d->rep1 = d->rep0;
            d->rep0 = dd;
        }
        len = lz_len(d, P + LZ_REP_LEN, ps);
        d->state = state < 7 ? 8 : 11;
    } else {
        d->rep3 = d->rep2;
        d->rep2 = d->rep1;
        d->rep1 = d->rep0;
        len = lz_len(d, P + LZ_LEN, ps);
        d->state = state < 7 ? 7 : 10;
        d->rep0 = lz_dist(d, len);
        if (d->rep0 >= d->out_pos) {   /* includes the end marker before the range is full */
            d->err = 0x72;
            return;
        }
    }
    len += 2;
    if (len > d->out_size - d->out_pos) {
        d->err = 0x72;
        return;
    }
    while (len--) put_byte(d, get_byte(d, d->rep0));
}

static void lz_reset(fe_lzd_t *d, uint8_t *out, uint32_t out_size, uint32_t lpmask, uint32_t pbmask)
{
    memset(d, 0, sizeof(*d));
    for (int i = 0; i < LZ_NPROBS; i++) d->P[i] = 1024;
    d->out = out;
    d->out_size = out_size;
    d->lpmask = lpmask;
    d->pbmask = pbmask;
}

/* Decode as far as the input received so far allows. 0 = needs more input, 1 = range full, -1 = err */
static int lz_run(fake_ecu_t *e, fe_lzd_t *d)
{
    d->in = e->lz_in;
    d->in_len = e->lz_in_len;
    for (;;) {
        if (d->err) return -1;
        if (d->out_pos >= d->out_size) return 1;
        uint32_t avail = d->in_len - d->in_pos;
        if (!d->started) {
            if (avail < 5) return 0;
            if (lz_in(d) != 0) d->err = 0x72;   /* a range-coder stream starts with 0 */
            d->range = 0xFFFFFFFFu;
            d->code = 0;
            for (int i = 0; i < 4; i++) d->code = (d->code << 8) | lz_in(d);
            if (d->code == d->range) d->err = 0x72;
            d->started = 1;
            continue;
        }
        if (avail >= LZ_SAFE_INPUT) {
            lz_step(d);
            if (d->starved) {
                e->decode_starved_live++;
                return -1;
            }
            continue;
        }
        /* The tail of the input: go on with a copy; keep it only if it ends without running dry. */
        fe_lzd_t *c = malloc(sizeof(*c));
        *c = *d;
        int outcome = 0;
        for (;;) {
            if (c->err) { outcome = -1; break; }
            if (c->out_pos >= c->out_size) { outcome = 1; break; }
            lz_step(c);
            if (c->starved) { outcome = 0; break; }
        }
        if (outcome != 0) *d = *c;
        free(c);
        return outcome;
    }
}

/* ================================================================================================ */
/* Fake ECU                                                                                         */
/* ================================================================================================ */

static void kernel_start(fake_ecu_t *e);

void fe_init(fake_ecu_t *e, const uint8_t *initial_flash, uint32_t *clock)
{
    memset(e, 0, sizeof(*e));
    memcpy(e->flash, initial_flash, FE_ROM);
    e->clock = clock;
    e->kver = 3;
    e->kcls = 1;
    e->kready = 1;
    e->loader_restart_ok = true;
    e->erase_ms_small = 300;
    e->erase_ms_big = 1500;
    e->page_us = 1000;
    e->probes_before_first_erase = -1;
    e->lz = malloc(sizeof(fe_lzd_t));
    e->lz_in = malloc(0x40000);
}

void fe_start_kernel(fake_ecu_t *e)
{
    kernel_start(e);
}

void fe_free(fake_ecu_t *e)
{
    free(e->lz);
    free(e->lz_in);
    e->lz = NULL;
    e->lz_in = NULL;
}

void fe_add_fault(fake_ecu_t *e, int kind, int nth, int action, uint32_t arg, int repeat)
{
    fe_fault_t *f = &e->faults[e->nfaults++];
    f->kind = kind;
    f->nth = nth;
    f->action = action;
    f->arg = arg;
    f->repeat = repeat;
}

static uint32_t now(fake_ecu_t *e) { return *e->clock; }

static bool in_outage(fake_ecu_t *e, uint32_t t)
{
    return e->outage_to > e->outage_from && t >= e->outage_from && t < e->outage_to;
}

static void push(fake_ecu_t *e, uint32_t t, const uint8_t *d, int len)
{
    if (e->cur_action == FA_DROP_ANSWER) return;
    if (e->cur_action == FA_LATE) t += e->cur_arg;
    if (in_outage(e, t)) return;
    if (e->qn >= 64) return;
    if (e->noise && e->qn < 62) {
        static const uint8_t n1[3] = { 0x7F, 0x22, 0x11 };
        static const uint8_t n2[4] = { 0x62, 0xF1, 0x90, 0x00 };
        fe_rsp_t a = { t, 3, { 0 } }, b = { t, 4, { 0 } };
        memcpy(a.d, n1, 3);
        memcpy(b.d, n2, 4);
        e->q[e->qn++] = a;
        e->q[e->qn++] = b;
    }
    for (int copy = 0; copy < (e->cur_action == FA_DUP ? 2 : 1) && e->qn < 64; copy++) {
        uint32_t tc = copy ? t + e->cur_arg : t;
        fe_rsp_t r = { tc, (uint8_t)len, { 0 } };
        memcpy(r.d, d, (size_t)len);
        /* keep the queue ordered by time */
        int i = e->qn;
        while (i > 0 && e->q[i - 1].t > tc) {
            e->q[i] = e->q[i - 1];
            i--;
        }
        e->q[i] = r;
        e->qn++;
    }
}

static void answer(fake_ecu_t *e, uint32_t delay, const uint8_t *d, int len) { push(e, now(e) + delay, d, len); }

static void neg(fake_ecu_t *e, uint32_t delay, uint8_t sid, uint8_t nrc)
{
    uint8_t r[3] = { 0x7F, sid, nrc };
    answer(e, delay, r, 3);
}

/* the work takes `ms`: pendings every ~4 s, then the answer */
static void answer_after(fake_ecu_t *e, uint32_t ms, uint8_t sid, const uint8_t *d, int len)
{
    if (e->cur_action == FA_SLOW) ms += e->cur_arg;
    for (uint32_t t = FE_PENDING_MS; t < ms; t += FE_PENDING_MS) neg(e, t, sid, 0x78);
    answer(e, ms ? ms : 1, d, len);
}

static uint32_t be(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

static bool blank(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != 0xFF) return false;
    return true;
}

static bool app_invalid(fake_ecu_t *e) { return e->last_erased && blank(e->flash + FE_ENTRY, 4); }

/* kernel.c program_page() */
static bool program_page(fake_ecu_t *e, uint32_t a, const uint8_t *data)
{
    if (a < FE_APP_START || a > FE_ROM - FE_PAGE || (a % FE_PAGE)) {
        e->low_requests++;
        return false;
    }
    if (!blank(e->flash + a, FE_PAGE)) return false;
    if (e->commit_programmed) e->programs_after_commit++;
    if (e->cur_action == FA_REFUSE) {   /* a programming failure: the page is left half written */
        memset(e->flash + a, 0x00, FE_PAGE / 2);
        e->cur_action = 0;
        return false;
    }
    memcpy(e->flash + a, data, FE_PAGE);
    return true;
}

/* kernel.c erase_block() */
static bool erase_block(fake_ecu_t *e, unsigned blk)
{
    if (blk < 2 || blk > 15) {
        e->low_requests++;
        return false;
    }
    uint32_t start = ff_block_start(blk), len = ff_block_start(blk + 1) - start;
    uint8_t cp[FE_PAGE];
    if (blk == 15) {
        e->last_erased = 0;
        memcpy(cp, e->flash + FE_COUNTER_PAGE, FE_PAGE);
    }
    memset(e->flash + start, 0xFF, len);
    e->erase_count[blk]++;
    if (blk == 15) {
        uint16_t w0 = (uint16_t)be(cp, 2), w1 = (uint16_t)be(cp + 2, 2);
        if (!(w0 == (uint16_t)~w1 && w0 == 0xFFFE)) w0 = 1;
        cp[0] = (uint8_t)(w0 >> 8);
        cp[1] = (uint8_t)w0;
        cp[2] = (uint8_t)(~w0 >> 8);
        cp[3] = (uint8_t)~w0;
        memcpy(cp + 4, "_WDS", 4);
        int keep = e->cur_action;
        e->cur_action = 0;
        bool ok = program_page(e, FE_COUNTER_PAGE, cp);
        e->cur_action = keep;
        if (!ok) return false;
        e->counter = w0;
        e->last_erased = 1;
    }
    return true;
}

static void kernel_start(fake_ecu_t *e)
{
    e->st = 1;
    uint16_t w0 = (uint16_t)be(e->flash + FE_COUNTER_PAGE, 2), w2 = (uint16_t)be(e->flash + FE_COUNTER_PAGE + 2, 2);
    e->counter = (w0 == (uint16_t)~w2 && w0 != 0xFFFF) ? w0 : 0;
    if (e->force_counter) e->counter = e->force_counter;
    e->flash_ready = e->kready;
    e->last_erased = 0;
    e->prog_open = e->lz_mode = e->lz_done = e->lz_seq = e->in_decode = 0;
}

/* ---- compressed range: kernel.c lz_transfer() / next_piece() ---- */

static void lz_flush_pages(fake_ecu_t *e, uint32_t *pages)
{
    fe_lzd_t *d = e->lz;
    while ((e->pages_done + 1) * FE_PAGE <= d->out_pos) {
        uint32_t off = e->pages_done * FE_PAGE, a = e->dec_base + off;
        const uint8_t *pg = d->out + off;
        bool ok = blank(pg, FE_PAGE) ? blank(e->flash + a, FE_PAGE) : program_page(e, a, pg);
        if (!ok) {
            d->err = 0x72;
            return;
        }
        e->pages_done++;
        e->prog_ptr += FE_PAGE;
        e->prog_left -= FE_PAGE;
        (*pages)++;
    }
}

static uint8_t *s_lz_out;   /* the range being decoded (at most one block) */

/* A piece for the decoder; answers as the kernel would. */
static void lz_feed(fake_ecu_t *e, const uint8_t *piece, uint32_t n)
{
    memcpy(e->lz_in + e->lz_in_len, piece, n);
    e->lz_in_len += n;
    e->pieces++;
    uint32_t pages = 0;
    int r = lz_run(e, e->lz);
    lz_flush_pages(e, &pages);
    if (e->cur_action == FA_REFUSE) {   /* a programming failure even if this piece filled no page */
        uint32_t a = e->dec_base + e->pages_done * FE_PAGE;
        if (e->pages_done * FE_PAGE < e->lz->out_size && blank(e->flash + a, FE_PAGE)) memset(e->flash + a, 0, 64);
        e->lz->err = 0x72;
        e->cur_action = 0;
    }
    if (e->lz->err) r = -1;
    uint32_t ms = pages * e->page_us / 1000 + 1;
    if (r < 0) {
        e->in_decode = 0;
        e->prog_open = 0;
        uint8_t nr[3] = { 0x7F, 0x36, (uint8_t)e->lz->err };
        answer_after(e, ms, 0x36, nr, 3);
        return;
    }
    uint8_t ok[2] = { 0x76, e->lz_seq };
    if (r > 0) {
        e->in_decode = 0;
        e->lz_done = 1;
    }
    answer_after(e, ms, 0x36, ok, 2);
}

static void svc_transfer_lz(fake_ecu_t *e, const uint8_t *rx, uint32_t n)
{
    if (e->in_decode) {                         /* next_piece() */
        if (n >= 2) {
            if (rx[1] == (uint8_t)(e->lz_seq + 1) && n >= 3) {
                e->lz_seq++;
                lz_feed(e, rx + 2, n - 2);
                return;
            }
            if (rx[1] == e->lz_seq) {
                uint8_t r[2] = { 0x76, e->lz_seq };
                answer(e, 1, r, 2);
                return;
            }
            e->in_decode = 0;
            e->prog_open = 0;
            neg(e, 1, 0x36, 0x24);
            return;
        }
        neg(e, 1, 0x36, 0x11);
        return;
    }
    if (!e->prog_open) {                        /* lz_transfer() */
        neg(e, 1, 0x36, 0x24);
        return;
    }
    if (n < 3) {
        e->prog_open = 0;
        neg(e, 1, 0x36, 0x31);
        return;
    }
    uint8_t r[2] = { 0x76, rx[1] };
    if (e->lz_seq != 0 && rx[1] == e->lz_seq) {
        answer(e, 1, r, 2);
        return;
    }
    if (rx[1] != (uint8_t)(e->lz_seq + 1)) {
        e->prog_open = 0;
        neg(e, 1, 0x36, 0x24);
        return;
    }
    e->lz_seq++;
    if (e->lz_done) {
        answer(e, 1, r, 2);
        return;
    }
    e->in_decode = 1;
    lz_feed(e, rx + 2, n - 2);
}

/* ---- the kernel's services ---- */

static void svc_routine(fake_ecu_t *e, const uint8_t *rx, uint32_t n)
{
    uint8_t r[7] = { 0x71, rx[1] };
    if (n == 2 && rx[1] == 0x00) {
        r[2] = 'N'; r[3] = 'F'; r[4] = e->kver; r[5] = e->kcls; r[6] = e->flash_ready;
        answer(e, 1, r, 7);
        return;
    }
    if (n == 8 && rx[1] == 0x01) {
        uint32_t a = be(rx + 2, 3), l = be(rx + 5, 3);
        if (l == 0 || a >= FE_ROM || l > FE_ROM - a) {
            neg(e, 1, 0x31, 0x31);
            return;
        }
        uint32_t c = ff_crc32(0, e->flash + a, l);
        r[2] = (uint8_t)(c >> 24); r[3] = (uint8_t)(c >> 16); r[4] = (uint8_t)(c >> 8); r[5] = (uint8_t)c;
        answer(e, 1 + l / 65536, r, 6);
        return;
    }
    if (n == 3 && rx[1] == 0x02) {
        unsigned b = rx[2];
        if (b < 2) e->low_requests++;
        if (b < 2 || b > 15) {
            neg(e, 1, 0x31, 0x31);
            return;
        }
        if (e->probes_before_first_erase < 0) e->probes_before_first_erase = e->counts[FK_PROBE];
        if (!e->flash_ready || e->counter >= 100 || (b != 15 && !app_invalid(e))) {
            neg(e, 1, 0x31, 0x22);
            return;
        }
        neg(e, 1, 0x31, 0x78);
        e->prog_open = 0;
        if (e->probes_before_first_erase < 0) e->probes_before_first_erase = e->counts[FK_PROBE];
        uint32_t ms = ff_block_start(b + 1) - ff_block_start(b) > 0x1000 ? e->erase_ms_big : e->erase_ms_small;
        if (!erase_block(e, b)) {
            neg(e, ms, 0x31, 0x72);
            return;
        }
        r[2] = (uint8_t)b;
        uint8_t keep[3] = { 0x71, 0x02, (uint8_t)b };
        (void)r;
        answer_after(e, ms, 0x31, keep, 3);
        return;
    }
    if (n == 4 && rx[1] == 0x03) {
        uint32_t l = be(rx + 2, 2);
        if (l == 0 || l > sizeof(e->kram)) {
            neg(e, 1, 0x31, 0x31);
            return;
        }
        uint32_t c = ff_crc32(0, e->kram, l);
        r[2] = (uint8_t)(c >> 24); r[3] = (uint8_t)(c >> 16); r[4] = (uint8_t)(c >> 8); r[5] = (uint8_t)c;
        answer(e, 1, r, 6);
        return;
    }
    neg(e, 1, 0x31, 0x12);
}

static void svc_download(fake_ecu_t *e, const uint8_t *rx, uint32_t n)
{
    static const uint8_t mask[3] = { 0, 1, 3 };
    uint32_t off = 1;
    int lz = 0;
    e->prog_open = 0;
    e->lz_mode = 0;
    if (n == 12 && rx[1] == 0x10) {
        if (rx[2] > 2 || rx[3] > 2) {
            neg(e, 1, 0x34, 0x31);
            return;
        }
        lz = 1;
        off = 4;
    } else if (n != 9) {
        neg(e, 1, 0x34, 0x12);
        return;
    }
    uint32_t a = be(rx + off, 4), l = be(rx + off + 4, 4);
    if (a < FE_APP_START) e->low_requests++;
    if (a < FE_APP_START || a >= FE_ROM || l == 0 || l > FE_ROM - a || (a % FE_PAGE) || (l % FE_PAGE)) {
        neg(e, 1, 0x34, 0x31);
        return;
    }
    if (a + l > FE_COMMIT_PAGE && !(a == FE_COMMIT_PAGE && l == FE_PAGE)) {
        neg(e, 1, 0x34, 0x31);
        return;
    }
    if (lz && (a == FE_COMMIT_PAGE || (a < FE_COUNTER_PAGE + FE_PAGE && a + l > FE_COUNTER_PAGE))) {
        neg(e, 1, 0x34, 0x31);
        return;
    }
    if (!e->flash_ready || !app_invalid(e)) {
        neg(e, 1, 0x34, 0x22);
        return;
    }
    e->prog_ptr = a;
    e->prog_left = l;
    e->prog_open = 1;
    if (lz) {
        e->lz_mode = 1;
        e->lz_seq = 0;
        e->lz_done = 0;
        e->in_decode = 0;
        e->dec_base = a;
        e->pages_done = 0;
        e->lz_in_len = 0;
        free(s_lz_out);
        s_lz_out = malloc(l);
        lz_reset(e->lz, s_lz_out, l, mask[rx[2]], mask[rx[3]]);
    }
    uint8_t r = 0x74;
    answer(e, 1, &r, 1);
}

static void svc_transfer_plain(fake_ecu_t *e, const uint8_t *rx, uint32_t len)
{
    uint32_t n = len - 1;
    if (!e->prog_open) {
        neg(e, 1, 0x36, 0x24);
        return;
    }
    if (n == 0 || (n % FE_PAGE) || n > e->prog_left) {
        e->prog_open = 0;
        neg(e, 1, 0x36, 0x31);
        return;
    }
    uint32_t pages = 0;
    for (uint32_t off = 0; off < n; off += FE_PAGE) {
        const uint8_t *b = rx + 1 + off;
        if (e->prog_ptr == FE_COMMIT_PAGE) e->last_erased = 0;
        if (e->prog_ptr != FE_COUNTER_PAGE) {
            bool is_commit = e->prog_ptr == FE_COMMIT_PAGE;
            bool good = false;
            if (is_commit && e->expect) {
                good = true;
                for (uint32_t a = FE_APP_START; a < FE_COMMIT_PAGE; a++)
                    if ((a < FE_COUNTER_PAGE || a >= FE_COUNTER_PAGE + FE_PAGE) && e->flash[a] != e->expect[a]) {
                        good = false;
                        break;
                    }
            }
            bool ok = blank(b, FE_PAGE) ? blank(e->flash + e->prog_ptr, FE_PAGE) : program_page(e, e->prog_ptr, b);
            if (!ok) {
                e->prog_open = 0;
                neg(e, 1 + pages, 0x36, 0x72);
                return;
            }
            if (is_commit) {
                e->commit_programmed++;
                if (good) e->commit_on_good_image++;
                if (e->cur_action == FA_LIE72) {   /* programmed, but the answer says it failed */
                    e->prog_open = 0;
                    neg(e, 1 + pages, 0x36, 0x72);
                    return;
                }
            }
            pages++;
        }
        e->prog_ptr += FE_PAGE;
        e->prog_left -= FE_PAGE;
    }
    uint8_t r = 0x76;
    answer(e, 1 + pages, &r, 1);
}

static void kernel_dispatch(fake_ecu_t *e, const uint8_t *rx, uint32_t n)
{
    uint8_t sid = rx[0];
    if (e->in_decode && sid != 0x36 && sid != 0x3E) {
        if (sid != 0x31 && sid != 0x34 && sid != 0x37 && sid != 0x11) {
            neg(e, 1, sid, 0x11);   /* v3: a foreign request does not end the stream */
            return;
        }
        e->in_decode = 0;           /* 31/34/37/11 end the range, then are served */
        e->prog_open = 0;
    }
    if (sid == 0x3E) {
        uint8_t r = 0x7E;
        answer(e, 1, &r, 1);
    } else if (sid == 0x31 && n >= 2) {
        svc_routine(e, rx, n);
    } else if (sid == 0x34) {
        svc_download(e, rx, n);
    } else if (sid == 0x36) {
        if (e->lz_mode) svc_transfer_lz(e, rx, n);
        else svc_transfer_plain(e, rx, n);
    } else if (sid == 0x37) {
        if (n != 1 || !e->prog_open || e->prog_left != 0) {
            neg(e, 1, 0x37, 0x24);
        } else {
            e->prog_open = 0;
            uint8_t r = 0x77;
            answer(e, 1, &r, 1);
        }
    } else if (sid == 0x11) {
        if (n != 2 || rx[1] != 0x01) {
            neg(e, 1, 0x11, 0x12);
        } else {
            uint8_t r = 0x51;
            answer(e, 1, &r, 1);
            e->st = 2;              /* halted: the supervisor resets the ECU */
            e->resets++;
        }
    } else {
        neg(e, 1, sid, 0x11);
    }
}

/* ---- the ROM loader, as far as the kernel upload goes ---- */
static void loader_dispatch(fake_ecu_t *e, const uint8_t *rx, uint32_t n)
{
    uint8_t sid = rx[0];
    if (sid == 0x34 && n == 9) {
        uint32_t a = be(rx + 1, 4), s = be(rx + 5, 4);
        if (e->dl_active && !e->loader_restart_ok) {
            neg(e, 1, 0x34, 0x22);
            return;
        }
        if (a != 0x8000 || s <= 0xFE000 || ((s - 0xFE000) % 0x400) || s - 0xFE000 > 0x3000) {
            neg(e, 1, 0x34, 0x31);
            return;
        }
        e->dl_active = 1;
        e->klen = s - 0xFE000;
        e->khave = 0;
        e->refuse_next_upload = 0;
        memset(e->kram, 0, sizeof(e->kram));
        uint8_t r = 0x74;
        answer(e, 1, &r, 1);
        return;
    }
    if (sid == 0x36) {
        if (!e->dl_active || n != 1 + 0x400) {
            neg(e, 1, 0x36, 0x24);
            return;
        }
        if (e->refuse_next_upload) {   /* bench 2026-11-15: a resend of a block it had taken */
            e->refuse_next_upload = 0;
            neg(e, 1, 0x36, 0x22);
            return;
        }
        memcpy(e->kram + e->khave, rx + 1, 0x400);
        if (e->cur_action == FA_CORRUPT) e->kram[e->khave + 0x123] ^= 0x01;
        e->khave += 0x400;
        if (e->cur_action == FA_DROP_ANSWER) e->refuse_next_upload = 1;
        if (e->khave == e->klen) {
            bool marker = false;
            for (uint32_t i = 0; i + 8 <= 0x400; i++)
                if (memcmp(e->kram + e->khave - 0x400 + i, "*SBL_END", 8) == 0) marker = true;
            if (marker) {
                neg(e, 1, 0x36, 0x78);
                kernel_start(e);
                uint8_t r = 0x76;
                answer(e, 300, &r, 1);   /* the kernel itself completes the last TransferData */
                return;
            }
        }
        uint8_t r = 0x76;
        answer(e, 2, &r, 1);
        return;
    }
    neg(e, 1, sid, 0x11);
}

static int classify(fake_ecu_t *e, const uint8_t *rx, uint32_t n)
{
    switch (rx[0]) {
    case 0x34: return e->st == 0 ? FK_DL : (n == 12 ? FK_OPENLZ : FK_OPEN);
    case 0x36: return e->st == 0 ? FK_UPLOAD : (e->lz_mode ? FK_PIECE : FK_PLAIN);
    case 0x37: return FK_CLOSE;
    case 0x11: return FK_RESET;
    case 0x3E: return FK_TP;
    case 0x31:
        if (n < 2) return FK_OTHER;
        if (rx[1] == 1 && n == 8 && be(rx + 2, 3) == FE_COMMIT_PAGE) return FK_CRCC;
        return rx[1] == 0 ? FK_IDENT : rx[1] == 1 ? FK_CRC : rx[1] == 2 ? FK_ERASE : rx[1] == 3 ? FK_KCRC
             : rx[1] == 0xFF ? FK_PROBE : FK_OTHER;
    default: return FK_OTHER;
    }
}

int fe_send(fake_ecu_t *e, const uint8_t *req, uint32_t len)
{
    static uint8_t rx[0x500];
    if (len == 0 || len > sizeof(rx)) return -0xE1;
    memcpy(rx, req, len);
    e->requests++;
    int kind = classify(e, rx, len);
    int nth = ++e->counts[kind];
    e->cur_kind = kind;
    e->cur_action = 0;
    e->cur_arg = 0;
    for (int i = 0; i < e->nfaults; i++) {
        fe_fault_t *f = &e->faults[i];
        if (f->kind == kind && (nth == f->nth || (f->repeat && nth >= f->nth))) {
            e->cur_action = f->action;
            e->cur_arg = f->arg;
        }
    }
    (*e->clock) += len > 7 ? 2 + len / 64 : 1;   /* the frames take time on the bus */
    if (e->cur_action == FA_HALT) return FF_IO_HALT;
    if (e->cur_action == FA_FC_FAIL) {
        (*e->clock) += 2000;
        return -0xE2;
    }
    if (e->cur_action == FA_DROP_REQUEST || in_outage(e, now(e))) return 0;
    if (e->cur_action == FA_CORRUPT && e->st != 0 && len > 40) rx[len / 2] ^= 0x5A;
    if (e->app_running) {
        neg(e, 1, rx[0], 0x11);   /* stand-in for "not our kernel" */
        return 0;
    }
    if (e->st == 0) loader_dispatch(e, rx, len);
    else if (e->st == 1) kernel_dispatch(e, rx, len);
    /* st 2: halted, no answer */
    return 0;
}

int fe_recv(fake_ecu_t *e, uint8_t *buf, uint32_t cap, uint32_t *len, uint32_t timeout_ms)
{
    uint32_t t = now(e);
    if (e->qn > 0 && e->q[0].t <= t + timeout_ms) {
        if (e->q[0].t > t) *e->clock = e->q[0].t;
        fe_rsp_t r = e->q[0];
        memmove(e->q, e->q + 1, sizeof(e->q[0]) * (size_t)(e->qn - 1));
        e->qn--;
        uint32_t n = r.len < cap ? r.len : cap;
        memcpy(buf, r.d, n);
        *len = n;
        return FF_IO_OK;
    }
    *e->clock = t + timeout_ms;
    return FF_IO_TIMEOUT;
}

void fe_flush(fake_ecu_t *e)
{
    uint32_t t = now(e);
    int k = 0;
    for (int i = 0; i < e->qn; i++)
        if (e->q[i].t > t) e->q[k++] = e->q[i];   /* not sent yet: stays */
    e->qn = k;
}
