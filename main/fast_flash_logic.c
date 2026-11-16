/*
 * Fast flash decision logic. See fast_flash_logic.h for the overview and
 * docs/internals/fast_flash.md for the wire contract. No ESP-IDF include may appear here: the host
 * tests (tools/hosttest/fast_flash_test.c) build this file with a stock C compiler.
 */
#include "fast_flash_logic.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================================ */
/* CRC-32 (zlib)                                                                                 */
/* ============================================================================================ */

static uint32_t s_crc_tab[256];
static bool s_crc_ready;

uint32_t ff_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    if (!s_crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            s_crc_tab[i] = c;
        }
        s_crc_ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = s_crc_tab[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

/* zlib crc32_combine(): the CRC of A||B from crc(A), crc(B) and len(B) (GF(2) matrix method). */
static uint32_t gf2_times(const uint32_t *mat, uint32_t vec)
{
    uint32_t sum = 0;
    while (vec) {
        if (vec & 1u) sum ^= *mat;
        vec >>= 1;
        mat++;
    }
    return sum;
}

static void gf2_square(uint32_t *sq, const uint32_t *mat)
{
    for (int n = 0; n < 32; n++) sq[n] = gf2_times(mat, mat[n]);
}

uint32_t ff_crc32_combine(uint32_t crc1, uint32_t crc2, uint32_t len2)
{
    uint32_t even[32], odd[32], row = 1;
    if (len2 == 0) return crc1;
    odd[0] = 0xEDB88320u;
    for (int n = 1; n < 32; n++) {
        odd[n] = row;
        row <<= 1;
    }
    gf2_square(even, odd);
    gf2_square(odd, even);
    do {
        gf2_square(even, odd);
        if (len2 & 1u) crc1 = gf2_times(even, crc1);
        len2 >>= 1;
        if (!len2) break;
        gf2_square(odd, even);
        if (len2 & 1u) crc1 = gf2_times(odd, crc1);
        len2 >>= 1;
    } while (len2);
    return crc1 ^ crc2;
}

/* The CRC the ECU must report for 0x2000..0xFFB00 (every application byte below the counter page)
   when it holds the image: combined from the manifest's per-range CRCs. */
static uint32_t ff_expected_app_crc(const ff_manifest_t *m)
{
    uint32_t c = 0;
    for (unsigned b = FF_FIRST_APP_BLOCK; b <= FF_LAST_BLOCK; b++)
        c = ff_crc32_combine(c, m->blk[b].pre[0].crc, m->blk[b].pre[0].len);
    return c;
}

/* ============================================================================================ */
/* Block map and the ranges derived from it (fast_flash.py BLOCK_STARTS, compare_ranges, _lz_ranges) */
/* ============================================================================================ */

static const uint32_t k_block_start[17] = {
    0x00000, 0x01000, 0x02000, 0x03000, 0x04000, 0x05000, 0x06000, 0x07000,
    0x08000, 0x20000, 0x40000, 0x60000, 0x80000, 0xA0000, 0xC0000, 0xE0000, 0x100000,
};

uint32_t ff_block_start(unsigned blk)
{
    return blk <= 16 ? k_block_start[blk] : FF_ROM_SIZE;
}

/* compare_ranges(block, with_commit): the counter page is never compared; just before the commit
   the commit page is left out too. */
static int ff_cmp_ranges(unsigned b, bool with_commit, ff_cmp_t out[2])
{
    uint32_t start = k_block_start[b], size = k_block_start[b + 1] - start;
    if (b != FF_LAST_BLOCK) {
        out[0].addr = start;
        out[0].len = size;
        return 1;
    }
    uint32_t end = with_commit ? start + size : FF_COMMIT_PAGE;
    uint32_t after = FF_COUNTER_PAGE + FF_PAGE;
    out[0].addr = start;
    out[0].len = FF_COUNTER_PAGE - start;
    out[1].addr = after;
    out[1].len = end - after;
    return 2;
}

/* _lz_ranges(block): the whole block; block 15 around the counter page and without the commit page */
static int ff_lz_ranges(unsigned b, ff_range_t out[2])
{
    uint32_t start = k_block_start[b], size = k_block_start[b + 1] - start;
    memset(out, 0, 2 * sizeof(out[0]));
    if (b != FF_LAST_BLOCK) {
        out[0].addr = start;
        out[0].size = size;
        return 1;
    }
    uint32_t after = FF_COUNTER_PAGE + FF_PAGE;
    out[0].addr = start;
    out[0].size = FF_COUNTER_PAGE - start;
    out[1].addr = after;
    out[1].size = FF_COMMIT_PAGE - after;
    return 2;
}

/* The hard rule for a compressed program range, independent of how it was derived: page aligned,
   at or above 0x2000, ending at or before the commit page, and clear of the counter page. */
static bool ff_lz_range_allowed(uint32_t a, uint32_t size)
{
    if (size == 0 || (a % FF_PAGE) || (size % FF_PAGE)) return false;
    if (a < FF_APP_START || a >= FF_COMMIT_PAGE) return false;
    if (size > FF_COMMIT_PAGE - a) return false;
    if (a < FF_COUNTER_PAGE + FF_PAGE && a + size > FF_COUNTER_PAGE) return false;
    return true;
}

/* ============================================================================================ */
/* A small strict JSON reader (the firmware's cJSON is not available to the host tests)          */
/* ============================================================================================ */

enum { J_OBJ = 1, J_ARR, J_NUM, J_STR, J_TRUE, J_FALSE, J_NULL };

typedef struct {
    uint8_t  t;
    int32_t  next, child;      /* sibling / first child, -1 = none */
    uint32_t ks, kl;           /* key (raw bytes in the text) when the parent is an object */
    uint32_t vs, vl;           /* string value (raw) */
    uint32_t num;
} jn_t;

typedef struct {
    const char *s;
    size_t n, i;
    jn_t *pool;
    int cap, used, depth;
} jp_t;

#define J_MAX_NODES 640
#define J_MAX_DEPTH 8

static void j_ws(jp_t *p)
{
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r'))
        p->i++;
}

static int j_str(jp_t *p, uint32_t *start, uint32_t *len)
{
    if (p->i >= p->n || p->s[p->i] != '"') return -1;
    p->i++;
    *start = (uint32_t)p->i;
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') {
            *len = (uint32_t)(p->i - *start);
            p->i++;
            return 0;
        }
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (p->i + 1 >= p->n) return -1;
            p->i += (p->s[p->i + 1] == 'u') ? 6 : 2;
            continue;
        }
        p->i++;
    }
    return -1;
}

static int j_val(jp_t *p);

static int j_alloc(jp_t *p, uint8_t t)
{
    if (p->used >= p->cap) return -1;
    jn_t *n = &p->pool[p->used];
    memset(n, 0, sizeof(*n));
    n->t = t;
    n->next = n->child = -1;
    return p->used++;
}

static int j_container(jp_t *p, bool obj)
{
    int me = j_alloc(p, obj ? J_OBJ : J_ARR);
    if (me < 0) return -1;
    p->i++; /* { or [ */
    j_ws(p);
    char close = obj ? '}' : ']';
    if (p->i < p->n && p->s[p->i] == close) {
        p->i++;
        return me;
    }
    int last = -1;
    for (;;) {
        uint32_t ks = 0, kl = 0;
        j_ws(p);
        if (obj) {
            if (j_str(p, &ks, &kl) != 0) return -1;
            j_ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') return -1;
            p->i++;
            j_ws(p);
        }
        int c = j_val(p);
        if (c < 0) return -1;
        p->pool[c].ks = ks;
        p->pool[c].kl = kl;
        if (last < 0) p->pool[me].child = c;
        else p->pool[last].next = c;
        last = c;
        j_ws(p);
        if (p->i >= p->n) return -1;
        if (p->s[p->i] == ',') {
            p->i++;
            continue;
        }
        if (p->s[p->i] == close) {
            p->i++;
            return me;
        }
        return -1;
    }
}

static int j_lit(jp_t *p, const char *lit, uint8_t t)
{
    size_t l = strlen(lit);
    if (p->n - p->i < l || memcmp(p->s + p->i, lit, l) != 0) return -1;
    p->i += l;
    return j_alloc(p, t);
}

static int j_val(jp_t *p)
{
    if (p->i >= p->n || ++p->depth > J_MAX_DEPTH) return -1;
    int r = -1;
    char c = p->s[p->i];
    if (c == '{' || c == '[') {
        r = j_container(p, c == '{');
    } else if (c == '"') {
        r = j_alloc(p, J_STR);
        if (r >= 0 && j_str(p, &p->pool[r].vs, &p->pool[r].vl) != 0) r = -1;
    } else if (c >= '0' && c <= '9') {
        /* non-negative integers only: the manifest carries nothing else */
        uint64_t v = 0;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') {
            v = v * 10u + (uint64_t)(p->s[p->i] - '0');
            if (v > 0xFFFFFFFFull) return -1;
            p->i++;
        }
        if (p->i < p->n && (p->s[p->i] == '.' || p->s[p->i] == 'e' || p->s[p->i] == 'E')) return -1;
        r = j_alloc(p, J_NUM);
        if (r >= 0) p->pool[r].num = (uint32_t)v;
    } else if (c == 't') {
        r = j_lit(p, "true", J_TRUE);
    } else if (c == 'f') {
        r = j_lit(p, "false", J_FALSE);
    } else if (c == 'n') {
        r = j_lit(p, "null", J_NULL);
    }
    p->depth--;
    return r;
}

static int j_get(const jp_t *p, int obj, const char *key)
{
    if (obj < 0 || p->pool[obj].t != J_OBJ) return -1;
    size_t kl = strlen(key);
    for (int c = p->pool[obj].child; c >= 0; c = p->pool[c].next)
        if (p->pool[c].kl == kl && memcmp(p->s + p->pool[c].ks, key, kl) == 0) return c;
    return -1;
}

static int j_u32(const jp_t *p, int obj, const char *key, uint32_t *out)
{
    int n = j_get(p, obj, key);
    if (n < 0 || p->pool[n].t != J_NUM) return -1;
    *out = p->pool[n].num;
    return 0;
}

static bool j_str_is(const jp_t *p, int n, const char *want)
{
    if (n < 0 || p->pool[n].t != J_STR) return false;
    size_t l = strlen(want);
    return p->pool[n].vl == l && memcmp(p->s + p->pool[n].vs, want, l) == 0;
}

static int j_count(const jp_t *p, int arr)
{
    int k = 0;
    for (int c = p->pool[arr].child; c >= 0; c = p->pool[c].next) k++;
    return k;
}

/* [[addr,len,crc], ...] with 1 or 2 entries */
static int j_cmp_list(const jp_t *p, int arr, ff_cmp_t out[2], uint8_t *n_out)
{
    if (arr < 0 || p->pool[arr].t != J_ARR) return -1;
    int n = j_count(p, arr);
    if (n < 1 || n > 2) return -1;
    int k = 0;
    for (int c = p->pool[arr].child; c >= 0; c = p->pool[c].next, k++) {
        if (p->pool[c].t != J_ARR || j_count(p, c) != 3) return -1;
        uint32_t v[3];
        int i = 0;
        for (int e = p->pool[c].child; e >= 0; e = p->pool[e].next, i++) {
            if (p->pool[e].t != J_NUM) return -1;
            v[i] = p->pool[e].num;
        }
        out[k].addr = v[0];
        out[k].len = v[1];
        out[k].crc = v[2];
    }
    *n_out = (uint8_t)n;
    return 0;
}

/* ============================================================================================ */
/* Manifest                                                                                      */
/* ============================================================================================ */

int ff_manifest_parse(const char *json, size_t len, ff_manifest_t *m, uint32_t *bad_addr)
{
    memset(m, 0, sizeof(*m));
    *bad_addr = 0;
    if (!json || len == 0 || len > FF_MANIFEST_MAX) return FF_ST_MANIFEST;
    jp_t p = { .s = json, .n = len, .cap = J_MAX_NODES };
    p.pool = (jn_t *)malloc(sizeof(jn_t) * J_MAX_NODES);
    if (!p.pool) return FF_ST_NOMEM;
    int rc = FF_ST_RULES;
    j_ws(&p);
    int root = j_val(&p);
    j_ws(&p);
    if (root < 0 || p.i != p.n || p.pool[root].t != J_OBJ) {
        rc = FF_ST_MANIFEST;
        goto out;
    }
    uint32_t mv = 0;
    if (!j_str_is(&p, j_get(&p, root, "kind"), "nc_fast_flash")) goto out;
    if (j_u32(&p, root, "manifest_version", &mv) || mv != 1) goto out;
    if (j_u32(&p, root, "package_len", &m->pkg_len) || j_u32(&p, root, "package_crc32", &m->pkg_crc)) goto out;

    int k = j_get(&p, root, "kernel");
    if (j_u32(&p, k, "off", &m->k_off) || j_u32(&p, k, "len", &m->k_len) ||
        j_u32(&p, k, "crc32", &m->k_crc) || j_u32(&p, k, "version", &m->k_ver)) goto out;

    int lz = j_get(&p, root, "lz");
    if (j_u32(&p, lz, "lp", &m->lp) || j_u32(&p, lz, "pb", &m->pb) || j_u32(&p, lz, "piece", &m->piece)) goto out;

    int bt = j_get(&p, root, "boot");
    if (j_u32(&p, bt, "crc32", &m->boot_crc)) goto out;
    int pol = j_get(&p, bt, "policy");
    if (pol < 0 || j_str_is(&p, pol, "refuse")) m->boot_allow = false;
    else if (j_str_is(&p, pol, "allow")) m->boot_allow = true;
    else goto out;

    int cm = j_get(&p, root, "commit");
    uint32_t caddr = 0;
    if (j_u32(&p, cm, "addr", &caddr) || j_u32(&p, cm, "off", &m->c_off) ||
        j_u32(&p, cm, "len", &m->c_len) || j_u32(&p, cm, "crc32", &m->c_crc)) goto out;
    if (caddr != FF_COMMIT_PAGE) {
        *bad_addr = caddr;
        goto out;
    }

    int rw = j_get(&p, root, "rewrite_all");
    if (rw >= 0) {
        if (p.pool[rw].t == J_TRUE) m->rewrite_all = true;
        else if (p.pool[rw].t != J_FALSE) goto out;
    }

    int blocks = j_get(&p, root, "blocks");
    if (blocks < 0 || p.pool[blocks].t != J_ARR) goto out;
    uint16_t seen = 0;
    for (int b = p.pool[blocks].child; b >= 0; b = p.pool[b].next) {
        uint32_t bn = 0;
        if (p.pool[b].t != J_OBJ || j_u32(&p, b, "block", &bn)) goto out;
        if (bn < FF_FIRST_APP_BLOCK || bn > FF_LAST_BLOCK || (seen & (1u << bn))) {
            *bad_addr = bn;
            goto out;
        }
        seen |= (uint16_t)(1u << bn);
        ff_block_t *B = &m->blk[bn];
        if (j_cmp_list(&p, j_get(&p, b, "cmp"), B->cmp, &B->ncmp)) {
            *bad_addr = bn;
            goto out;
        }
        int pre = j_get(&p, b, "cmp_pre");
        if (pre >= 0) {
            if (j_cmp_list(&p, pre, B->pre, &B->npre)) {
                *bad_addr = bn;
                goto out;
            }
        } else if (bn != FF_LAST_BLOCK) {
            memcpy(B->pre, B->cmp, sizeof(B->pre));
            B->npre = B->ncmp;
        } else {
            *bad_addr = bn;   /* block 15 must say how it reads before the commit */
            goto out;
        }
        int rg = j_get(&p, b, "ranges");
        if (rg < 0 || p.pool[rg].t != J_ARR) {
            *bad_addr = bn;
            goto out;
        }
        int nr = j_count(&p, rg);
        if (nr < 1 || nr > 2) {
            *bad_addr = bn;
            goto out;
        }
        int i = 0;
        for (int r = p.pool[rg].child; r >= 0; r = p.pool[r].next, i++) {
            ff_range_t *R = &B->r[i];
            if (p.pool[r].t != J_OBJ || j_u32(&p, r, "addr", &R->addr) || j_u32(&p, r, "size", &R->size) ||
                j_u32(&p, r, "off", &R->off) || j_u32(&p, r, "len", &R->len) || j_u32(&p, r, "crc32", &R->crc)) {
                *bad_addr = bn;
                goto out;
            }
        }
        B->nr = (uint8_t)nr;
    }
    if (seen != 0xFFFCu) { /* every block 2..15, exactly once */
        *bad_addr = seen;
        goto out;
    }
    rc = 0;
out:
    free(p.pool);
    return rc;
}

static bool ff_section_ok(uint32_t off, uint32_t len, uint32_t pkg_len)
{
    return len > 0 && off <= pkg_len && len <= pkg_len - off;
}

/* validate_kernel(): whole 0x400 blocks up to 0x3000, the end marker in the last block and its tail
   nowhere before it (fast_flash.py validate_kernel) */
static bool ff_kernel_image_ok(const uint8_t *k, uint32_t len)
{
    static const char mark[] = "*SBL_END";
    const uint32_t ml = 8;
    if (len == 0 || len % FF_KERNEL_BLOCK || len > FF_KERNEL_MAX) return false;
    const uint8_t *last = k + len - FF_KERNEL_BLOCK;
    bool in_last = false;
    for (uint32_t i = 0; i + ml <= FF_KERNEL_BLOCK; i++)
        if (memcmp(last + i, mark, ml) == 0) in_last = true;
    if (!in_last) return false;
    for (uint32_t i = 0; i + (ml - 1) <= len - FF_KERNEL_BLOCK; i++)
        if (memcmp(k + i, mark + 1, ml - 1) == 0) return false;
    return true;
}

int ff_manifest_validate(const ff_manifest_t *m, const uint8_t *pkg, uint32_t pkg_len, uint32_t *bad_addr)
{
    *bad_addr = 0;
    if (!pkg || pkg_len != m->pkg_len || pkg_len > FF_PACKAGE_MAX) {
        *bad_addr = pkg_len;
        return FF_ST_SIZE;
    }
    /* --- the rules first: what may reach the ECU --- */
    if (!ff_section_ok(m->k_off, m->k_len, pkg_len) || !ff_kernel_image_ok(pkg + m->k_off, m->k_len) ||
        m->k_ver < FF_KERNEL_MIN_VER || m->k_ver > 255) {
        *bad_addr = m->k_off;
        return FF_ST_RULES;
    }
    if (m->lp > 2 || m->pb > 2 || m->piece == 0 || m->piece > FF_LZ_PIECE_MAX) return FF_ST_RULES;
    for (unsigned b = FF_FIRST_APP_BLOCK; b <= FF_LAST_BLOCK; b++) {
        const ff_block_t *B = &m->blk[b];
        ff_cmp_t want[2];
        int n = ff_cmp_ranges(b, true, want);
        *bad_addr = k_block_start[b];
        if (B->ncmp != n) return FF_ST_RULES;
        for (int i = 0; i < n; i++)
            if (B->cmp[i].addr != want[i].addr || B->cmp[i].len != want[i].len) return FF_ST_RULES;
        n = ff_cmp_ranges(b, false, want);
        if (B->npre != n) return FF_ST_RULES;
        for (int i = 0; i < n; i++)
            if (B->pre[i].addr != want[i].addr || B->pre[i].len != want[i].len) return FF_ST_RULES;
        ff_range_t lz[2];
        n = ff_lz_ranges(b, lz);
        if (B->nr != n) return FF_ST_RULES;
        for (int i = 0; i < n; i++) {
            const ff_range_t *R = &B->r[i];
            *bad_addr = R->addr;
            if (R->addr != lz[i].addr || R->size != lz[i].size) return FF_ST_RULES;
            if (!ff_lz_range_allowed(R->addr, R->size)) return FF_ST_RULES;   /* belt and braces */
            if (!ff_section_ok(R->off, R->len, pkg_len) || R->len < 5) return FF_ST_RULES;
            if (pkg[R->off] != 0) return FF_ST_RULES;                         /* a range-coder stream starts with 0 */
        }
    }
    *bad_addr = FF_COMMIT_PAGE;
    if (m->c_len != FF_PAGE || !ff_section_ok(m->c_off, m->c_len, pkg_len)) return FF_ST_RULES;
    {
        const uint8_t *e = pkg + m->c_off + FF_PAGE - 4;
        if (e[0] == 0xFF && e[1] == 0xFF && e[2] == 0xFF && e[3] == 0xFF) return FF_ST_RULES; /* no entry pointer */
    }
    /* The compare CRCs and the commit page must describe ONE image (a packer that mixed two would
       commit another image's entry pointer onto a verified ROM): blocks 2..14 read the same before
       and after the commit, block 15's first range too, and its second range "after" is exactly its
       "before" followed by the commit page in the package. */
    for (unsigned b = FF_FIRST_APP_BLOCK; b < FF_LAST_BLOCK; b++) {
        *bad_addr = k_block_start[b];
        if (m->blk[b].pre[0].crc != m->blk[b].cmp[0].crc) return FF_ST_RULES;
    }
    *bad_addr = FF_COMMIT_PAGE;
    if (m->blk[FF_LAST_BLOCK].pre[0].crc != m->blk[FF_LAST_BLOCK].cmp[0].crc) return FF_ST_RULES;
    if (ff_crc32(m->blk[FF_LAST_BLOCK].pre[1].crc, pkg + m->c_off, FF_PAGE) != m->blk[FF_LAST_BLOCK].cmp[1].crc)
        return FF_ST_RULES;
    /* --- then every CRC --- */
    *bad_addr = 0;
    if (ff_crc32(0, pkg, pkg_len) != m->pkg_crc) return FF_ST_CRC;
    *bad_addr = m->k_off;
    if (ff_crc32(0, pkg + m->k_off, m->k_len) != m->k_crc) return FF_ST_CRC;
    for (unsigned b = FF_FIRST_APP_BLOCK; b <= FF_LAST_BLOCK; b++)
        for (int i = 0; i < m->blk[b].nr; i++) {
            const ff_range_t *R = &m->blk[b].r[i];
            *bad_addr = R->addr;
            if (ff_crc32(0, pkg + R->off, R->len) != R->crc) return FF_ST_CRC;
        }
    *bad_addr = FF_COMMIT_PAGE;
    if (ff_crc32(0, pkg + m->c_off, m->c_len) != m->c_crc) return FF_ST_CRC;
    *bad_addr = 0;
    return 0;
}

/* ============================================================================================ */
/* Lines to the PC, status, keepalive                                                            */
/* ============================================================================================ */

static uint32_t ff_now(ff_t *ff) { return ff->ops.now_ms(ff->ops.ctx); }

static bool ff_after(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; } /* a at or after b */

static void ff_line(ff_t *ff, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(ff->line, sizeof(ff->line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    int r = ff->ops.emit(ff->ops.ctx, ff->line, ff->st.ponr);
    ff->last_line_ms = ff_now(ff);
    if (r != 0 && !ff->st.ponr) ff->host_gone = true;
}

static void ff_status_changed(ff_t *ff)
{
    if (ff->ops.on_status) ff->ops.on_status(ff->ops.ctx, &ff->st);
}

static void ff_phase(ff_t *ff, const char *phase)
{
    snprintf(ff->st.phase, sizeof(ff->st.phase), "%s", phase);
    ff_status_changed(ff);
    ff_line(ff, "NCFFSTEP %s\n", phase);
}

/* A line every FF_KEEPALIVE_MS whatever the ECU is doing, so the PC's idle timer never fires. */
static void ff_beat(ff_t *ff)
{
    if (ff_after(ff_now(ff), ff->last_line_ms + FF_KEEPALIVE_MS))
        ff_line(ff, "NCFFPROG %lu/%lu blk=%d\n", (unsigned long)ff->st.done, (unsigned long)ff->st.total, ff->st.blk);
}

static void ff_sleep(ff_t *ff, uint32_t ms)
{
    while (ms) {
        uint32_t s = ms > FF_SLICE_MS ? FF_SLICE_MS : ms;
        ff->ops.sleep_ms(ff->ops.ctx, s);
        ms -= s;
        ff_beat(ff);
    }
}

/* ============================================================================================ */
/* One exchange                                                                                  */
/* ============================================================================================ */

/* Send one request and wait for its answer: the positive answer of THIS service (payload after
   the SID into ff->rsp), a refusal of THIS service, or nothing. "7F <sid> 78" extends the wait by
   pending_ms. A negative answer naming another service, or a positive answer to another service,
   belongs to another tester on the bus: it is counted and ignored. Late answers to earlier
   requests are dropped before sending (review 2026-11-15 finding 5). */
static int ff_req(ff_t *ff, const uint8_t *req, uint32_t len, uint32_t timeout_ms, uint32_t pending_ms)
{
    if (ff->host_gone && !ff->st.ponr && !ff->force_io) return FF_R_ABORT;
    ff->sub = 0;
    ff->nrc = 0;
    ff->ops.flush(ff->ops.ctx);
    int s = ff->ops.send(ff->ops.ctx, req, len);
    if (s == FF_IO_HALT) {
        ff->halted = true;
        return FF_R_HALT;
    }
    if (s != FF_IO_OK) {
        ff->sub = (s <= -0xE1 && s >= -0xEF) ? (uint8_t)(-s) : FF_SUB_SEND;
        return FF_R_LINK;
    }
    const uint8_t sid = req[0];
    uint32_t t0 = ff_now(ff);
    uint32_t deadline = t0 + timeout_ms;
    const uint32_t ceiling = t0 + FF_REQ_CEILING_MS;
    for (;;) {
        ff_beat(ff);
        if (ff->host_gone && !ff->st.ponr && !ff->force_io) return FF_R_ABORT;
        uint32_t now = ff_now(ff);
        if (ff_after(now, deadline)) {
            ff->sub = FF_SUB_NO_ANS;
            return FF_R_LINK;
        }
        uint32_t w = deadline - now;
        if (w > FF_SLICE_MS) w = FF_SLICE_MS;
        uint8_t b[8];
        uint32_t bl = 0;
        int r = ff->ops.recv(ff->ops.ctx, b, sizeof(b), &bl, w);
        if (r == FF_IO_HALT) {
            ff->halted = true;
            return FF_R_HALT;
        }
        if (r != FF_IO_OK || bl == 0) continue;
        if (b[0] == 0x7F) {
            if (bl < 3 || b[1] != sid) {
                ff->foreign++;
                continue;
            }
            if (b[2] == 0x78) {
                ff->pendings++;
                deadline = ff_now(ff) + pending_ms;
                if (ff_after(deadline, ceiling)) deadline = ceiling;
                continue;
            }
            ff->nrc = b[2];
            return FF_R_NRC;
        }
        /* A duplicate answer to the PREVIOUS piece that landed after this one went out (review
           2026-11-16 finding 6): not ours, keep waiting. */
        if (sid == 0x36 && b[0] == 0x76 && bl == 2 && ff->stale_seq >= 0 && b[1] == (uint8_t)ff->stale_seq) {
            ff->foreign++;
            continue;
        }
        if (b[0] == (uint8_t)(sid + 0x40)) {
            ff->rsp_len = bl - 1;
            memcpy(ff->rsp, b + 1, ff->rsp_len);
            return FF_R_OK;
        }
        ff->foreign++;
    }
}

/* _retry(): an idempotent request, tried again after a link error. After `tries` quick retries the
   link is waited out for up to FF_LINK_OUTAGE_MS; tries <= 1 (a probe) never waits it out. */
static int ff_req_retry(ff_t *ff, const uint8_t *req, uint32_t len, uint32_t timeout_ms, uint32_t pending_ms,
                        unsigned tries)
{
    unsigned attempt = 0;
    bool outage = false;
    uint32_t outage_deadline = 0;
    for (;;) {
        int r = ff_req(ff, req, len, timeout_ms, pending_ms);
        if (r != FF_R_LINK) return r;
        attempt++;
        if (attempt <= tries) {
            ff_sleep(ff, FF_LINK_RETRY_WAIT_MS);
            continue;
        }
        if (tries <= 1) return r;
        uint32_t now = ff_now(ff);
        if (!outage) {
            outage = true;
            outage_deadline = now + FF_LINK_OUTAGE_MS;
            ff->outages++;
            ff_line(ff, "NCFFNOTE outage sid=%02X a=%06lX\n", req[0], (unsigned long)ff->cur_addr);
        }
        if (ff_after(now, outage_deadline)) return r;
        ff_sleep(ff, FF_LINK_OUTAGE_WAIT_MS);
    }
}

static void put_be(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
}

static uint32_t get_be(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

/* ============================================================================================ */
/* Kernel services                                                                               */
/* ============================================================================================ */

/* ecu_crc(): CRC-32 of [a, a + n) of the ECU's ROM. Reading is harmless anywhere. */
static int ff_ecu_crc(ff_t *ff, uint32_t a, uint32_t n, uint32_t *out)
{
    if (n == 0 || a >= FF_ROM_SIZE || n > FF_ROM_SIZE - a) return FF_R_FATAL;
    uint8_t *q = ff->req;
    q[0] = 0x31;
    q[1] = 0x01;
    put_be(q + 2, a, 3);
    put_be(q + 5, n, 3);
    ff->cur_addr = a;
    int r = ff_req_retry(ff, q, 8, FF_TIMEOUT_CRC_MS, FF_PENDING_DEFAULT_MS, FF_WRITE_RETRIES);
    if (r) return r;
    if (ff->rsp_len != 5 || ff->rsp[0] != 0x01) {
        ff->sub = FF_SUB_BAD_ANS;
        return FF_R_FATAL;
    }
    *out = get_be(ff->rsp + 1, 4);
    return FF_R_OK;
}

/* erase_block(): blocks 2..15 only, whatever the caller asks. */
static int ff_erase_block(ff_t *ff, unsigned b)
{
    ff->stage_hint = FF_ST_ERASE;
    ff->cur_addr = ff_block_start(b);
    if (b < FF_FIRST_APP_BLOCK || b > FF_LAST_BLOCK || ff_block_start(b) < FF_APP_START) {
        ff->sub = FF_SUB_BAD_ANS;
        return FF_R_FATAL;
    }
    uint8_t *q = ff->req;
    q[0] = 0x31;
    q[1] = 0x02;
    q[2] = (uint8_t)b;
    int r = ff_req_retry(ff, q, 3, FF_TIMEOUT_ERASE_MS, FF_TIMEOUT_ERASE_MS, FF_WRITE_RETRIES);
    if (r) return r;
    if (ff->rsp_len != 2 || ff->rsp[0] != 0x02 || ff->rsp[1] != b) {
        ff->sub = FF_SUB_BAD_ANS;
        return FF_R_FATAL;
    }
    return FF_R_OK;
}

/* identify(): "71 00 'N' 'F' version class ready" */
static int ff_identify(ff_t *ff, unsigned tries)
{
    uint8_t q[2] = { 0x31, 0x00 };
    ff->cur_addr = 0;
    int r = ff_req_retry(ff, q, 2, FF_TIMEOUT_IDENTIFY_MS, FF_TIMEOUT_IDENTIFY_MS, tries);
    if (r) return r;
    if (ff->rsp_len != 6 || ff->rsp[0] != 0x00 || ff->rsp[1] != 'N' || ff->rsp[2] != 'F' ||
        ff->rsp[3] < 1 || ff->rsp[3] > 3) {
        ff->sub = FF_SUB_BAD_ANS;
        return FF_R_FATAL;
    }
    ff->kver = ff->rsp[3];
    ff->kcls = ff->rsp[4];
    ff->kready = ff->rsp[5] == 1;
    return FF_R_OK;
}

/* running_kernel(): one probe. OK = our kernel answers; HALT/ABORT propagate; anything else = none. */
static int ff_running_kernel(ff_t *ff)
{
    return ff_identify(ff, 1);
}

/* verify_kernel_image(): the kernel as it sits in ECU RAM must be the file that was sent. Every
   guard protecting the boot block lives in that RAM image. */
static int ff_verify_kernel(ff_t *ff)
{
    uint8_t q[4] = { 0x31, 0x03, (uint8_t)(ff->m.k_len >> 8), (uint8_t)ff->m.k_len };
    ff->cur_addr = FF_KERNEL_LOAD_ADDR;
    int r = ff_req_retry(ff, q, 4, FF_TIMEOUT_CRC_MS, FF_PENDING_DEFAULT_MS, FF_WRITE_RETRIES);
    if (r) return r;
    if (ff->rsp_len != 5 || ff->rsp[0] != 0x03) {
        ff->sub = FF_SUB_BAD_ANS;
        return FF_R_FATAL;
    }
    if (get_be(ff->rsp + 1, 4) != ff->m.k_crc) {
        ff->sub = FF_SUB_MISMATCH;
        return FF_R_FATAL;
    }
    return FF_R_OK;
}

/* The 1 KB link probe (review 2026-11-16 finding 1): before anything is erased, prove that the
   running kernel receives a full-size request (147 back-to-back frames into its single receive
   mailbox), as every compressed piece will be. "31 FF" + 1022 bytes is refused "7F 31 12" by the
   kernel's routine service and changes nothing; it also ends a compressed range a cut-short op may
   have left open (R), exactly like the identify did. Never a bare 36: that would feed an open range. */
static int ff_probe(ff_t *ff)
{
    for (unsigned k = 0; k < FF_PROBE_COUNT; k++) {
        bool got = false;
        for (unsigned a = 0; a < FF_PROBE_TRIES && !got; a++) {
            uint8_t *q = ff->req;
            q[0] = 0x31;
            q[1] = FF_PROBE_SUBFN;
            for (unsigned i = 2; i < FF_PROBE_LEN; i++) q[i] = (uint8_t)(i * 7u + k);
            ff->cur_addr = 0;
            int r = ff_req(ff, q, FF_PROBE_LEN, FF_TIMEOUT_IDENTIFY_MS, FF_TIMEOUT_IDENTIFY_MS);
            if (r == FF_R_HALT || r == FF_R_ABORT) return r;
            if (r == FF_R_NRC && ff->nrc == 0x12) {
                got = true;
                break;
            }
            ff->probe_lost++;
            if (r == FF_R_OK || r == FF_R_NRC) {   /* an answer, but not the kernel's refusal */
                ff->sub = FF_SUB_BAD_ANS;
                return FF_R_FATAL;
            }
            ff_sleep(ff, FF_LINK_RETRY_WAIT_MS);
        }
        if (!got) {
            ff->sub = FF_SUB_NO_ANS;
            return FF_R_LINK;
        }
    }
    ff->probe_ok = true;
    return FF_R_OK;
}

#define FF_OUT_OF_STEP 1

/* _upload_blocks(): a block without an answer is sent again; if the loader refuses the resend it
   had taken the block, and the caller starts over. */
static int ff_upload_blocks(ff_t *ff)
{
    const uint8_t *k = ff->pkg + ff->m.k_off;
    for (uint32_t off = 0; off < ff->m.k_len; off += FF_KERNEL_BLOCK) {
        bool resent = false;
        ff->cur_addr = off;
        for (unsigned attempt = 0; attempt <= FF_WRITE_RETRIES; attempt++) {
            ff->req[0] = 0x36;
            memcpy(ff->req + 1, k + off, FF_KERNEL_BLOCK);
            int r = ff_req(ff, ff->req, 1 + FF_KERNEL_BLOCK, FF_TIMEOUT_PROGRAM_MS, FF_TIMEOUT_PROGRAM_MS);
            if (r == FF_R_OK) break;
            if (r == FF_R_NRC) return resent ? FF_OUT_OF_STEP : r;
            if (r != FF_R_LINK || attempt == FF_WRITE_RETRIES) return r;
            resent = true;
            ff->resends++;
            ff_line(ff, "NCFFNOTE resend kernel a=%04lX\n", (unsigned long)off);
            ff_sleep(ff, FF_LINK_RETRY_WAIT_MS);
        }
    }
    return FF_R_OK;
}

/* upload_kernel(). *stage = the FFERR stage on failure. */
static int ff_upload_kernel(ff_t *ff, int *stage)
{
    for (unsigned restart = 0; restart <= FF_UPLOAD_RESTARTS; restart++) {
        uint8_t *q = ff->req;
        q[0] = 0x34;
        put_be(q + 1, FF_KERNEL_LOAD_ADDR, 4);
        put_be(q + 5, FF_LOADER_SIZE_BASE + ff->m.k_len, 4);
        ff->cur_addr = FF_KERNEL_LOAD_ADDR;
        int r = ff_req(ff, q, 9, FF_TIMEOUT_DEFAULT_MS, FF_PENDING_DEFAULT_MS);
        if (r == FF_R_NRC && restart > 0) break;   /* the loader will not start over in this session */
        if (r == FF_R_NRC) {
            /* Our kernel refuses this RequestDownload (length out of range): is it already running,
               left by an op that was cut short? Then it is a resume (R), not a fresh flash. */
            uint8_t nrc = ff->nrc;
            int k = ff_running_kernel(ff);
            if (k == FF_R_HALT || k == FF_R_ABORT) {
                *stage = FF_ST_DOWNLOAD;
                return k;
            }
            if (k == FF_R_OK) {
                ff->kernel_running = true;
                ff->kernel_preexisting = true;
                *stage = FF_ST_KERNEL_RUNNING;
                ff->sub = FF_SUB_BAD_ANS;
                return FF_R_FATAL;
            }
            ff->nrc = nrc;
            *stage = FF_ST_DOWNLOAD;
            return FF_R_NRC;
        }
        if (r) {
            *stage = FF_ST_DOWNLOAD;
            return r;
        }
        r = ff_upload_blocks(ff);
        if (r == FF_R_OK) return FF_R_OK;
        if (r != FF_OUT_OF_STEP) {
            *stage = FF_ST_UPLOAD;
            return r;
        }
        /* An answer was lost and the resend refused: the loader had taken the block. If it was the
           last one, the kernel is running. */
        r = ff_running_kernel(ff);
        if (r == FF_R_OK) return FF_R_OK;
        if (r == FF_R_HALT || r == FF_R_ABORT) {
            *stage = FF_ST_UPLOAD;
            return r;
        }
        ff->restarts++;
        ff_line(ff, "NCFFNOTE upload_restart n=%u\n", restart + 1);
    }
    *stage = FF_ST_UPLOAD_LOST;
    ff->sub = FF_SUB_BAD_ANS;
    return FF_R_FATAL;
}

/* _close_range(): if the 77 was lost and the resend is refused as out of sequence, the first one
   had closed the range. */
static int ff_close_range(ff_t *ff)
{
    bool resent = false;
    ff->stage_hint = FF_ST_CLOSE;
    for (unsigned attempt = 0; attempt <= FF_WRITE_RETRIES; attempt++) {
        uint8_t q[1] = { 0x37 };
        int r = ff_req(ff, q, 1, FF_TIMEOUT_PROGRAM_MS, FF_PENDING_DEFAULT_MS);
        if (r == FF_R_OK) return r;
        if (r == FF_R_NRC) return (resent && ff->nrc == 0x24) ? FF_R_OK : r;
        if (r != FF_R_LINK || attempt == FF_WRITE_RETRIES) return r;
        resent = true;
        ff->resends++;
        ff_sleep(ff, FF_LINK_RETRY_WAIT_MS);
    }
    return FF_R_LINK;
}

/* _open_range() for the plain path. The firmware sends exactly one plain range: the commit page. */
static int ff_open_range_plain(ff_t *ff, uint32_t a, uint32_t size)
{
    ff->stage_hint = FF_ST_COMMIT;
    ff->cur_addr = a;
    if (a < FF_COMMIT_PAGE || a + size != FF_ROM_SIZE || (a % FF_PAGE) || size == 0 || (size % FF_PAGE)) {
        ff->sub = FF_SUB_BAD_ANS;
        return FF_R_FATAL;
    }
    uint8_t *q = ff->req;
    q[0] = 0x34;
    put_be(q + 1, a, 4);
    put_be(q + 5, size, 4);
    return ff_req_retry(ff, q, 9, FF_TIMEOUT_PROGRAM_MS, FF_TIMEOUT_PROGRAM_MS, FF_WRITE_RETRIES);
}

/* _send_range(): a write without an answer is recovered without ever writing at the wrong address
   (the range is re-opened at that chunk; if the chunk had landed the kernel refuses the second write
   and the CRC decides). */
static int ff_send_range_plain(ff_t *ff, uint32_t address, uint32_t size, const uint8_t *data)
{
    int r = ff_open_range_plain(ff, address, size);
    if (r) return r;
    uint32_t off = 0;
    bool is_open = true;
    while (off < size) {
        uint32_t a = address + off;
        uint32_t n = size - off > FF_KERNEL_BLOCK ? FF_KERNEL_BLOCK : size - off;
        bool retried = false;
        for (unsigned attempt = 0; attempt <= FF_WRITE_RETRIES; attempt++) {
            ff->req[0] = 0x36;
            memcpy(ff->req + 1, data + off, n);
            ff->cur_addr = a;
            ff->stage_hint = FF_ST_COMMIT;
            r = ff_req(ff, ff->req, 1 + n, FF_TIMEOUT_WRITE_MS, FF_TIMEOUT_WRITE_MS);
            if (r == FF_R_OK) break;
            if (r == FF_R_NRC) {
                uint8_t nrc = ff->nrc;
                if (!retried || nrc != 0x72) return r;
                uint32_t c = 0;
                int r2 = ff_ecu_crc(ff, a, n, &c);
                if (r2) return r2;
                if (c != ff_crc32(0, data + off, n)) {
                    ff->nrc = nrc;
                    return FF_R_NRC;
                }
                ff_line(ff, "NCFFNOTE landed a=%06lX\n", (unsigned long)a);
                if (off + n < size) {
                    r2 = ff_open_range_plain(ff, a + n, size - off - n);
                    if (r2) return r2;
                } else {
                    is_open = false; /* the kernel closed the range on the refusal */
                }
                break;
            }
            if (r != FF_R_LINK || attempt == FF_WRITE_RETRIES) return r;
            ff->resends++;
            ff_sleep(ff, FF_LINK_RETRY_WAIT_MS);
            r = ff_open_range_plain(ff, a, size - off);
            if (r) return r;
            retried = true;
        }
        off += n;
    }
    return is_open ? ff_close_range(ff) : FF_R_OK;
}

/* _send_lz_range(): open a compressed range, then pieces "36 seq stream" answered "76 seq". A piece
   whose answer was lost is sent again with the same seq: the kernel answers it again without using
   it twice. A refusal returns FF_R_NRC; the caller erases the block again and resends it whole. */
static int ff_send_lz_range(ff_t *ff, const ff_range_t *R)
{
    ff->stage_hint = FF_ST_OPEN;
    ff->cur_addr = R->addr;
    if (!ff_lz_range_allowed(R->addr, R->size)) {
        ff->sub = FF_SUB_BAD_ANS;
        return FF_R_FATAL;
    }
    uint8_t *q = ff->req;
    q[0] = 0x34;
    q[1] = 0x10;
    q[2] = (uint8_t)ff->m.lp;
    q[3] = (uint8_t)ff->m.pb;
    put_be(q + 4, R->addr, 4);
    put_be(q + 8, R->size, 4);
    int r = ff_req_retry(ff, q, 12, FF_TIMEOUT_PROGRAM_MS, FF_TIMEOUT_PROGRAM_MS, FF_WRITE_RETRIES);
    if (r) return r;
    const uint8_t *stream = ff->pkg + R->off;
    uint8_t seq = 0;
    ff->stage_hint = FF_ST_PIECE;
    for (uint32_t off = 0; off < R->len; off += ff->m.piece) {
        uint32_t n = R->len - off > ff->m.piece ? ff->m.piece : R->len - off;
        ff->stale_seq = seq ? seq : -1;   /* the previous piece of THIS range */
        seq = (uint8_t)(seq + 1);
        q[0] = 0x36;
        q[1] = seq;
        memcpy(q + 2, stream + off, n);
        ff->cur_addr = R->addr;
        r = ff_req_retry(ff, q, 2 + n, FF_TIMEOUT_LZ_PIECE_MS, FF_TIMEOUT_ERASE_MS, FF_WRITE_RETRIES);
        if (r) {
            ff->stale_seq = -1;
            return r;
        }
        if (ff->rsp_len != 1 || ff->rsp[0] != seq) {
            ff->sub = FF_SUB_BAD_ANS;
            return FF_R_FATAL;
        }
        ff->bytes_sent += n;
    }
    ff->stale_seq = -1;
    return ff_close_range(ff);
}

/* program_block_lz(): a refused transfer gets one more try after erasing the block again. */
static int ff_program_block_lz(ff_t *ff, unsigned b)
{
    if (b < FF_FIRST_APP_BLOCK || b > FF_LAST_BLOCK) return FF_R_FATAL;
    for (int attempt = 0; attempt < 2; attempt++) {
        int r = FF_R_OK;
        for (int i = 0; i < ff->m.blk[b].nr && r == FF_R_OK; i++) r = ff_send_lz_range(ff, &ff->m.blk[b].r[i]);
        if (r != FF_R_NRC || attempt) return r;
        ff->reerases++;
        ff_line(ff, "NCFFNOTE reerase blk=%u nrc=%02X\n", b, ff->nrc);
        uint8_t nrc = ff->nrc;
        r = ff_erase_block(ff, b);
        if (r) return r;
        ff->nrc = nrc;
    }
    return FF_R_OK; /* not reached */
}

/* compare_blocks(): per block, stop at the first range that differs (like Python's all()). */
static int ff_compare_blocks(ff_t *ff, bool with_commit, uint16_t *differs, uint32_t crcs[16][2])
{
    *differs = 0;
    for (unsigned b = FF_FIRST_APP_BLOCK; b <= FF_LAST_BLOCK; b++) {
        const ff_block_t *B = &ff->m.blk[b];
        const ff_cmp_t *c = with_commit ? B->cmp : B->pre;
        int n = with_commit ? B->ncmp : B->npre;
        ff->st.blk = (int)b;
        for (int i = 0; i < n; i++) {
            uint32_t got = 0;
            int r = ff_ecu_crc(ff, c[i].addr, c[i].len, &got);
            if (r) return r;
            crcs[b][i] = got;
            if (got != c[i].crc) {
                *differs |= (uint16_t)(1u << b);
                break;
            }
        }
    }
    ff->st.blk = 0;
    return FF_R_OK;
}

/* commit(): program the commit page and verify it; erase block 15 again ONLY when the page is known
   wrong (a CRC that answered and does not match). A lost answer never erases a good flash.
   Returns 0, FF_R_HALT, or the FFERR stage (60 wrong page, 61 link lost while checking it). */
static int ff_commit(ff_t *ff)
{
    const uint8_t *page = ff->pkg + ff->m.c_off;
    bool refused = false;
    int r = ff_send_range_plain(ff, FF_COMMIT_PAGE, FF_PAGE, page);
    if (r == FF_R_HALT) return r;
    if (r == FF_R_NRC) {
        refused = true;
        ff_line(ff, "NCFFNOTE commit_refused nrc=%02X\n", ff->nrc);
    } else if (r == FF_R_LINK) {
        ff_line(ff, "NCFFNOTE commit_no_answer\n");
    } else if (r != FF_R_OK) {
        /* not a link error and not a refusal (an answer that makes no sense): the CRC still decides */
        refused = true;
    }
    uint8_t nrc_keep = ff->nrc;
    uint32_t got = 0;
    int rc = ff_ecu_crc(ff, FF_COMMIT_PAGE, FF_PAGE, &got);
    if (rc == FF_R_HALT) return rc;
    if (rc != FF_R_OK) {
        ff->err_nrc = rc == FF_R_NRC ? ff->nrc : ff->sub;
        (void)refused;
        return FF_ST_COMMIT_LINK;   /* unknown: the ECU may already be complete; flash again to confirm */
    }
    /* A refusal "72" means the kernel's own read-back of the page failed: even if the CRC now
       matches, the cells are marginal, so the page is not trusted (review 2026-11-16 finding 7; the
       PC reference trusts the CRC here). */
    bool marginal = refused && nrc_keep == 0x72;
    if (got == ff->m.c_crc && !marginal) return 0;
    ff->err_nrc = refused ? nrc_keep : FF_SUB_MISMATCH;
    int e = ff_erase_block(ff, FF_LAST_BLOCK);
    if (e == FF_R_HALT) return e;
    if (e != FF_R_OK) {
        /* The page reads wrong (or marginal) and block 15 could not be erased again: the ECU may
           hold a programmed entry pointer on a wrong page. The kernel still runs: R repairs it (the
           entry reads valid, block 15 differs, so it is erased and rewritten). Do not cycle the
           ignition first. */
        ff_line(ff, "NCFFNOTE commit_reerase_failed\n");
        return FF_ST_COMMIT_STUCK;
    }
    return FF_ST_COMMIT;
}

/* reset_ecu(): best effort, one try; the kernel answers 51 and stops, the supervisor resets the ECU. */
static int ff_reset(ff_t *ff)
{
    uint8_t q[2] = { 0x11, 0x01 };
    ff->force_io = true;
    int r = ff_req(ff, q, 2, FF_TIMEOUT_IDENTIFY_MS, FF_TIMEOUT_IDENTIFY_MS);
    ff->force_io = false;
    ff->kernel_running = false;
    ff_line(ff, "NCFFNOTE reset %s\n", r == FF_R_OK ? "ok" : "no_answer");
    return r == FF_R_HALT ? r : FF_R_OK;
}

/* ============================================================================================ */
/* Result                                                                                        */
/* ============================================================================================ */

static int ff_mask_list(char *o, size_t cap, uint16_t mask)
{
    size_t n = 0;
    int w = snprintf(o, cap, "[");
    n += (size_t)(w > 0 ? w : 0);
    bool first = true;
    for (unsigned b = 0; b < 16; b++)
        if (mask & (1u << b)) {
            w = snprintf(o + n, n < cap ? cap - n : 0, "%s%u", first ? "" : ",", b);
            n += (size_t)(w > 0 ? w : 0);
            first = false;
        }
    w = snprintf(o + n, n < cap ? cap - n : 0, "]");
    n += (size_t)(w > 0 ? w : 0);
    return (int)n;
}

static void ff_mask_csv(char *o, size_t cap, uint16_t mask)
{
    size_t n = 0;
    o[0] = 0;
    for (unsigned b = 0; b < 16 && n < cap; b++)
        if (mask & (1u << b)) {
            int w = snprintf(o + n, cap - n, "%s%u", n ? "," : "", b);
            if (w > 0) n += (size_t)w;
        }
    if (!n) snprintf(o, cap, "-");
}

static void ff_build_result(ff_t *ff)
{
    char diff[64], rew[64];
    ff_mask_list(diff, sizeof(diff), ff->diff_mask);
    ff_mask_list(rew, sizeof(rew), ff->rewrite_mask);
    ff->ms_total = ff_now(ff) - ff->t0_ms;
    snprintf(ff->result_json, sizeof(ff->result_json),
             "{\"v\":1,\"mode\":\"%c\",\"ok\":%d,\"nothing\":%d,\"kv\":%u,\"cls\":%u,\"ready\":%u,\"resumed\":%d,"
             "\"boot_same\":%d,\"cut\":%d,\"diff\":%s,\"rewritten\":%s,\"bytes\":%lu,\"resends\":%lu,"
             "\"reerases\":%lu,\"restarts\":%lu,\"outages\":%lu,\"foreign\":%lu,\"pendings\":%lu,"
             "\"ms\":{\"kernel\":%lu,\"compare\":%lu,\"write\":%lu,\"verify\":%lu,\"commit\":%lu,\"total\":%lu},"
             "\"probe\":{\"ok\":%d,\"lost\":%lu},\"app\":\"%c\","
             "\"err\":{\"st\":%d,\"a\":%lu,\"nrc\":%u,\"blk\":%d,\"ponr\":%d}}",
             ff->st.mode, ff->ok ? 1 : 0, ff->nothing_to_do ? 1 : 0, ff->kver, ff->kcls, ff->kready,
             ff->resumed ? 1 : 0, ff->boot_same ? 1 : 0, ff->cut_short ? 1 : 0, diff, rew,
             (unsigned long)ff->bytes_sent, (unsigned long)ff->resends, (unsigned long)ff->reerases,
             (unsigned long)ff->restarts, (unsigned long)ff->outages, (unsigned long)ff->foreign,
             (unsigned long)ff->pendings, (unsigned long)ff->ms_kernel, (unsigned long)ff->ms_compare,
             (unsigned long)ff->ms_write, (unsigned long)ff->ms_verify, (unsigned long)ff->ms_commit,
             (unsigned long)ff->ms_total, ff->probe_ok ? 1 : 0, (unsigned long)ff->probe_lost, ff_app_state(ff),
             ff->err_stage, (unsigned long)ff->err_addr, ff->err_nrc, ff->err_blk,
             ff->err_ponr ? 1 : 0);

    size_t n = 0;
    const size_t cap = sizeof(ff->crc_json);
    int w = snprintf(ff->crc_json, cap, "{\"before\":[");
    n += (size_t)(w > 0 ? w : 0);
    for (int pass = 0; pass < 2; pass++) {
        uint32_t (*t)[2] = pass ? ff->crc_after : ff->crc_before;
        for (unsigned b = FF_FIRST_APP_BLOCK; b <= FF_LAST_BLOCK && n < cap; b++) {
            w = snprintf(ff->crc_json + n, cap - n, "%s[%u,%lu,%lu]", b == FF_FIRST_APP_BLOCK ? "" : ",", b,
                         (unsigned long)t[b][0], (unsigned long)t[b][1]);
            n += (size_t)(w > 0 ? w : 0);
        }
        if (n < cap) {
            w = snprintf(ff->crc_json + n, cap - n, pass ? "]}" : "],\"after\":[");
            n += (size_t)(w > 0 ? w : 0);
        }
    }
}

static int ff_finish_fail(ff_t *ff, int stage, int r)
{
    if (r == FF_R_HALT || ff->halted) {
        ff->halted = true;
        ff->err_stage = 0;
        snprintf(ff->st.phase, sizeof(ff->st.phase), "halted");
        ff_status_changed(ff);
        return -1;
    }
    if (r == FF_R_ABORT) {
        stage = FF_ST_HOST_GONE;
        ff->err_nrc = 0;   /* no ECU code: a sub-code from an earlier, recovered retry would mislead */
    }
    ff->err_stage = stage;
    if (!ff->err_addr) ff->err_addr = ff->cur_addr;
    if (!ff->err_nrc && r != FF_R_ABORT) ff->err_nrc = r == FF_R_NRC ? ff->nrc : ff->sub;
    if (!ff->err_blk) ff->err_blk = ff->st.blk;
    /* ponr = "the ROM may be touched". Past THIS op's point of no return, yes. A kernel that was
       already running before this op (mode R, or st 26) belongs to an op that may have been cut
       after ITS point of no return: unless this op read a programmed entry pointer, the ROM may be
       half written. In both cases the kernel is kept up so R can finish the job. */
    bool rom_may_be_touched =
        ff->st.ponr || (ff->kernel_preexisting && !(ff->entry_known && !ff->cut_short));
    ff->err_ponr = rom_may_be_touched;
    /* Otherwise the ROM is as this op found it: if our kernel runs and the ECU has an application,
       let it restart. An ECU this op found with its entry pointer erased (no application) keeps
       the kernel, so R can rewrite it without a new session. */
    bool no_app = ff->entry_known && ff->cut_short;
    if (!rom_may_be_touched && ff->kernel_running && !no_app) {
        if (ff_reset(ff) == FF_R_HALT) return ff_finish_fail(ff, stage, FF_R_HALT);
    }
    ff_build_result(ff);
    snprintf(ff->st.phase, sizeof(ff->st.phase), "failed");
    ff_status_changed(ff);
    ff_line(ff, "NCFFRES %s\n", ff->result_json);
    ff_line(ff, "\r\nFFERR a=%06lX st=%d nrc=%02X blk=%d ponr=%d app=%c\r\n", (unsigned long)ff->err_addr, stage,
            ff->err_nrc, ff->err_blk, ff->err_ponr ? 1 : 0, ff_app_state(ff));
    return stage;
}

char ff_app_state(const ff_t *ff)
{
    if (ff->st.ponr) {
        if (ff->ok) return '1';
        /* 61: the commit page may be in; 62: a wrong page may be in */
        return (ff->err_stage == FF_ST_COMMIT_LINK || ff->err_stage == FF_ST_COMMIT_STUCK) ? 'u' : '0';
    }
    if (ff->entry_known) return ff->cut_short ? '0' : '1';
    return 'u';
}

static int ff_finish_ok(ff_t *ff)
{
    ff->ok = true;
    ff_build_result(ff);
    snprintf(ff->st.phase, sizeof(ff->st.phase), "done");
    ff_status_changed(ff);
    ff_line(ff, "NCFFRES %s\n", ff->result_json);
    ff_line(ff, "NCFFDONE mode=%c nothing=%d rewritten=%lu\n", ff->st.mode, ff->nothing_to_do ? 1 : 0,
            (unsigned long)ff->st.done);
    return 0;
}

/* ============================================================================================ */
/* Entry points                                                                                  */
/* ============================================================================================ */

void ff_init(ff_t *ff, const ff_ops_t *ops, char mode, bool allow_live)
{
    memset(ff, 0, sizeof(*ff));
    ff->ops = *ops;
    ff->st.mode = mode;
    ff->allow_live = allow_live;
    snprintf(ff->st.phase, sizeof(ff->st.phase), "load");
    ff->t0_ms = ff_now(ff);
    ff->last_line_ms = ff->t0_ms;
    ff->stale_seq = -1;
}

void ff_fail_early(ff_t *ff, int stage, uint32_t addr)
{
    ff->err_stage = stage;
    ff->err_addr = addr;
    ff_build_result(ff);
    snprintf(ff->st.phase, sizeof(ff->st.phase), "failed");
    ff_status_changed(ff);
    ff_line(ff, "\r\nFFERR a=%06lX st=%d nrc=00 blk=0 ponr=0 app=u\r\n", (unsigned long)addr, stage);
}

int ff_check(ff_t *ff, const char *json, size_t json_len, const uint8_t *pkg, uint32_t pkg_len)
{
    uint32_t bad = 0;
    ff->checked = false;
    int st = ff_manifest_parse(json, json_len, &ff->m, &bad);
    if (!st) st = ff_manifest_validate(&ff->m, pkg, pkg_len, &bad);
    if (st) {
        ff_fail_early(ff, st, bad);
        return st;
    }
    ff->pkg = pkg;
    ff->pkg_len = pkg_len;
    ff->checked = true;
    return 0;
}

int ff_run(ff_t *ff)
{
    char m = ff->st.mode;
    int r, stage = 0;
    uint32_t t;
    if (!ff->checked) {
        ff_fail_early(ff, FF_ST_RULES, 0);
        return FF_ST_RULES;
    }
    if (m != 'D' && m != 'C' && m != 'L' && m != 'R') {
        ff_fail_early(ff, FF_ST_COMMAND, 0);
        return FF_ST_COMMAND;
    }
    if ((m == 'L' || m == 'R') && !ff->allow_live) {
        ff_fail_early(ff, FF_ST_LIVE_GATE, 0);
        return FF_ST_LIVE_GATE;
    }
    if (m == 'D') return ff_finish_ok(ff);   /* package checked, nothing sent on CAN */

    /* ---- S1 kernel ------------------------------------------------------------------------ */
    ff_phase(ff, "kernel");
    t = ff_now(ff);
    if (m == 'R') {
        r = ff_running_kernel(ff);
        if (r) return ff_finish_fail(ff, FF_ST_IDENTIFY, r);   /* no kernel: see app=u, journal */
        ff->resumed = true;
        ff->kernel_preexisting = true;
    } else {
        r = ff_upload_kernel(ff, &stage);
        if (r) return ff_finish_fail(ff, stage, r);
        r = ff_identify(ff, FF_WRITE_RETRIES);
        if (r) return ff_finish_fail(ff, FF_ST_IDENTIFY, r);
    }
    ff->kernel_running = true;
    ff_line(ff, "NCFFKERN v=%u cls=%u ready=%u resumed=%d\n", ff->kver, ff->kcls, ff->kready, ff->resumed ? 1 : 0);
    if (ff->kver != ff->m.k_ver || ff->kver < FF_KERNEL_MIN_VER) {
        ff->sub = FF_SUB_BAD_ANS;
        return ff_finish_fail(ff, FF_ST_KVERSION, FF_R_FATAL);
    }
    r = ff_verify_kernel(ff);
    if (r) return ff_finish_fail(ff, FF_ST_KCRC, r);
    /* The link proof, before anything can be erased: full-size requests into the running kernel. */
    r = ff_probe(ff);
    ff_line(ff, "NCFFPROBE ok=%d lost=%lu\n", ff->probe_ok ? 1 : 0, (unsigned long)ff->probe_lost);
    if (r) return ff_finish_fail(ff, FF_ST_PROBE, r);
    ff->ms_kernel = ff_now(ff) - t;

    /* ---- S2 compare ----------------------------------------------------------------------- */
    ff_phase(ff, "compare");
    t = ff_now(ff);
    r = ff_compare_blocks(ff, true, &ff->diff_mask, ff->crc_before);
    if (r) return ff_finish_fail(ff, FF_ST_COMPARE, r);
    uint32_t c = 0;
    r = ff_ecu_crc(ff, 0, FF_APP_START, &c);
    if (r) return ff_finish_fail(ff, FF_ST_COMPARE, r);
    ff->boot_same = c == ff->m.boot_crc;
    static const uint8_t erased[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    r = ff_ecu_crc(ff, FF_ENTRY_POINTER, 4, &c);
    if (r) return ff_finish_fail(ff, FF_ST_COMPARE, r);
    ff->cut_short = c == ff_crc32(0, erased, 4);
    ff->entry_known = true;
    /* An ECU without an application: journal it now, so a dongle restart or a failure before this
       op's own point of no return still reports "interrupted" (the journal is only removed when a
       live op settles with the ECU bootable or untouched). */
    if (ff->cut_short && (m == 'L' || m == 'R') && ff->ops.persist) ff->ops.persist(ff->ops.ctx, "no_app");
    ff->ms_compare = ff_now(ff) - t;

    uint16_t change = ff->diff_mask;
    if (ff->cut_short || ff->m.rewrite_all) change = 0xFFFCu;   /* every application block */
    {
        char d[48], w[48];
        ff_mask_csv(d, sizeof(d), ff->diff_mask);
        ff_mask_csv(w, sizeof(w), m == 'C' ? 0 : (change ? (uint16_t)(change | (1u << FF_LAST_BLOCK)) : 0));
        ff_line(ff, "NCFFCMP diff=%s boot=%d cut=%d rewrite=%s\n", d, ff->boot_same ? 1 : 0,
                ff->cut_short ? 1 : 0, w);
    }
    if (m == 'C') {
        ff_phase(ff, "reset");
        if (ff_reset(ff) == FF_R_HALT) return ff_finish_fail(ff, 0, FF_R_HALT);
        return ff_finish_ok(ff);
    }
    if (!ff->boot_same && !ff->m.boot_allow) {
        ff->sub = FF_SUB_MISMATCH;
        ff->cur_addr = 0;
        return ff_finish_fail(ff, FF_ST_BOOT, FF_R_FATAL);
    }
    if (!change) {
        ff->nothing_to_do = true;
        ff_phase(ff, "reset");
        if (ff_reset(ff) == FF_R_HALT) return ff_finish_fail(ff, 0, FF_R_HALT);
        return ff_finish_ok(ff);
    }
    if (!ff->kready) {
        ff->sub = FF_SUB_BAD_ANS;
        return ff_finish_fail(ff, FF_ST_NOT_READY, FF_R_FATAL);
    }
    /* Last look before the first erase: the package in RAM must still be the one that was checked
       (CRCs again), and the PC must still be there. */
    {
        uint32_t bad = 0;
        int v = ff_manifest_validate(&ff->m, ff->pkg, ff->pkg_len, &bad);
        if (v) {
            ff->cur_addr = bad;
            ff->sub = FF_SUB_MISMATCH;
            return ff_finish_fail(ff, v, FF_R_FATAL);
        }
    }
    ff_beat(ff);
    if (ff->host_gone) return ff_finish_fail(ff, FF_ST_HOST_GONE, FF_R_ABORT);

    /* ---- POINT OF NO RETURN: from the block-15 erase until the commit page verifies, the ECU has no
       application. Nothing the PC does stops the flash from here; only the ECU can. ------------- */
    if (ff->ops.persist) ff->ops.persist(ff->ops.ctx, "ponr");
    change |= (uint16_t)(1u << FF_LAST_BLOCK);
    for (unsigned b = 0; b < 16; b++)
        if (change & (1u << b)) ff->st.total++;
    ff->st.ponr = true;
    ff->stale_seq = -1;
    ff_phase(ff, "write");
    t = ff_now(ff);
    ff->st.blk = FF_LAST_BLOCK;
    r = ff_erase_block(ff, FF_LAST_BLOCK);
    if (r == FF_R_NRC && ff->nrc != 0x72) {
        /* Refused outright (flash counter >= 100, flash not ready): the kernel checks that before it
           touches anything and answers before any 78, and no earlier attempt can have erased (an
           erase only lowers the counter to 1). Nothing was erased: this is still before the point
           of no return, so the ECU goes back to its application (review 2026-11-16 finding 3). */
        ff->st.ponr = false;
        ff_status_changed(ff);
        return ff_finish_fail(ff, FF_ST_ERASE15_REFUSED, r);
    }
    if (r) return ff_finish_fail(ff, FF_ST_ERASE, r);
    for (unsigned b = FF_FIRST_APP_BLOCK; b < FF_LAST_BLOCK; b++) {
        if (!(change & (1u << b))) continue;
        ff->st.blk = (int)b;
        ff_status_changed(ff);
        ff_line(ff, "NCFFPROG %lu/%lu blk=%u\n", (unsigned long)ff->st.done, (unsigned long)ff->st.total, b);
        r = ff_erase_block(ff, b);
        if (r) return ff_finish_fail(ff, FF_ST_ERASE, r);
        r = ff_program_block_lz(ff, b);
        if (r) return ff_finish_fail(ff, ff->stage_hint, r);
        ff->rewrite_mask |= (uint16_t)(1u << b);
        ff->st.done++;
    }
    ff->st.blk = FF_LAST_BLOCK;
    ff_status_changed(ff);
    ff_line(ff, "NCFFPROG %lu/%lu blk=%u\n", (unsigned long)ff->st.done, (unsigned long)ff->st.total, FF_LAST_BLOCK);
    r = ff_program_block_lz(ff, FF_LAST_BLOCK);
    if (r) return ff_finish_fail(ff, ff->stage_hint, r);
    ff->rewrite_mask |= (uint16_t)(1u << FF_LAST_BLOCK);
    ff->st.done++;
    ff_line(ff, "NCFFPROG %lu/%lu blk=0\n", (unsigned long)ff->st.done, (unsigned long)ff->st.total);
    ff->ms_write = ff_now(ff) - t;

    /* ---- S6 verify everything BEFORE the commit page: a bad block can never be started ---- */
    ff_phase(ff, "verify");
    t = ff_now(ff);
    uint16_t bad = 0;
    r = ff_compare_blocks(ff, false, &bad, ff->crc_after);
    if (r) return ff_finish_fail(ff, FF_ST_VERIFY_LINK, r);
    if (!bad) {
        /* One more CRC over everything below the counter page (0x2000..0xFFB00), against the value
           combined from the manifest: a stale answer read for one range (a CRC answer carries no
           address) cannot also fake this one (review 2026-11-16 finding 6). */
        uint32_t whole = 0;
        r = ff_ecu_crc(ff, FF_APP_START, FF_COUNTER_PAGE - FF_APP_START, &whole);
        if (r) return ff_finish_fail(ff, FF_ST_VERIFY_LINK, r);
        if (whole != ff_expected_app_crc(&ff->m)) {
            ff->st.blk = 0;
            ff->cur_addr = FF_APP_START;
            ff->sub = FF_SUB_MISMATCH;
            return ff_finish_fail(ff, FF_ST_VERIFY, FF_R_FATAL);
        }
    }
    ff->ms_verify = ff_now(ff) - t;
    if (bad) {
        for (unsigned b = 0; b < 16; b++)
            if (bad & (1u << b)) {
                ff->st.blk = (int)b;
                ff->cur_addr = ff_block_start(b);
                break;
            }
        ff->sub = FF_SUB_MISMATCH;
        return ff_finish_fail(ff, FF_ST_VERIFY, FF_R_FATAL);
    }

    /* ---- S7 commit ----------------------------------------------------------------------- */
    ff_phase(ff, "commit");
    t = ff_now(ff);
    ff->st.blk = FF_LAST_BLOCK;
    int cs = ff_commit(ff);
    if (cs == FF_R_HALT) return ff_finish_fail(ff, 0, FF_R_HALT);
    if (cs) {
        ff->cur_addr = FF_COMMIT_PAGE;
        return ff_finish_fail(ff, cs, FF_R_FATAL);
    }
    ff->ms_commit = ff_now(ff) - t;

    /* ---- S8 reset: the ECU starts its new application ------------------------------------- */
    ff->st.blk = 0;
    ff_phase(ff, "reset");
    if (ff_reset(ff) == FF_R_HALT) return ff_finish_fail(ff, 0, FF_R_HALT);
    return ff_finish_ok(ff);
}
