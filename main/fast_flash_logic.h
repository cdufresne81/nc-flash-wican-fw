/*
 * Fast flash: the decision logic, free of ESP-IDF so tools/hosttest can run it against a fake ECU.
 *
 * The dongle rewrites only the ECU flash blocks that differ from a ROM image, through the
 * nc-flash-re "fast-flash kernel" (v3), from a package the PC built and uploaded to the SD card.
 * This file is a port of the PC reference implementation (nc-rom-editor src/ecu/fast_flash.py,
 * bench-proven over a Tactrix 2026-11-15) and mirrors its sequence and retry rules:
 *
 *   upload_kernel / _upload_blocks  -> ff_upload_kernel / ff_upload_blocks
 *   identify, verify_kernel_image   -> ff_identify, ff_verify_kernel
 *   compare_blocks, ecu_crc         -> ff_compare_blocks, ff_ecu_crc
 *   erase_block                     -> ff_erase_block
 *   _send_lz_range, program_block_lz-> ff_send_lz_range, ff_program_block_lz
 *   _send_range, _close_range       -> ff_send_range_plain, ff_close_range
 *   commit                          -> ff_commit
 *   fast_flash / compare_only       -> ff_run
 *
 * The ECU side (wire protocol) is documented in docs/internals/fast_flash.md and in the header of
 * the kernel source (nc-flash-re tools/fast_flash/kernel/kernel.c).
 *
 * What this module never does, whatever the package says (checked in ff_check and again in every
 * request builder): erase a block outside 2..15, open a program range below 0x2000, put image data
 * on the counter page 0xFFB00-0xFFB7F or the commit page 0xFFF80, or program the commit page before
 * every other block has been read back by CRC and matched.
 *
 * The I/O is a small ops table (send one ISO-TP request, receive one single-frame answer with a
 * timeout, drop late answers, clock, sleep, emit a status line, journal). The ESP implementation is
 * main/ncflash_fastflash.c; the test one is tools/hosttest/fast_flash_test.c.
 */
#ifndef FAST_FLASH_LOGIC_H
#define FAST_FLASH_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- ROM layout (SH7058, LF-VE NC ECU) ------------------------------------------------------- */
#define FF_ROM_SIZE          0x100000u
#define FF_APP_START         0x2000u      /* blocks 0 and 1 hold the boot loader: never written */
#define FF_FIRST_APP_BLOCK   2u
#define FF_LAST_BLOCK        15u
#define FF_COUNTER_PAGE      0xFFB00u     /* the kernel's flash counter; never image data */
#define FF_PAGE              128u
#define FF_COMMIT_PAGE       0xFFF80u     /* programming it makes the application bootable */
#define FF_ENTRY_POINTER     0xFFFFCu
#define FF_KERNEL_LOAD_ADDR  0x8000u      /* RequestDownload address the ROM loader expects */
#define FF_LOADER_SIZE_BASE  0xFE000u     /* the loader computes kernel length = size - this */
#define FF_KERNEL_BLOCK      0x400u
#define FF_KERNEL_MAX        0x3000u
#define FF_KERNEL_MIN_VER    3u           /* self CRC (31 03) + block-15-first rule for every range */
#define FF_LZ_PIECE_MAX      1022u        /* 0x36 + seq + 1022 = one 1024-byte request */
#define FF_MANIFEST_MAX      8192u
#define FF_PACKAGE_MAX       (2u * 1024u * 1024u)

/* ---- timing, mirrored from fast_flash.py ----------------------------------------------------- */
#define FF_TIMEOUT_ERASE_MS     30000u
#define FF_TIMEOUT_CRC_MS       10000u
#define FF_TIMEOUT_PROGRAM_MS   10000u
#define FF_TIMEOUT_WRITE_MS      4000u
#define FF_TIMEOUT_LZ_PIECE_MS   6000u
#define FF_TIMEOUT_IDENTIFY_MS   2000u
#define FF_TIMEOUT_DEFAULT_MS    5000u   /* first answer, when the reference gives no timeout */
#define FF_PENDING_DEFAULT_MS    5000u   /* after a 7F xx 78, when the reference gives none */
#define FF_REQ_CEILING_MS      300000u   /* no single request waits longer, pendings included */
#define FF_WRITE_RETRIES            4u
#define FF_UPLOAD_RESTARTS          2u
#define FF_LINK_RETRY_WAIT_MS    1500u   /* the kernel drops a half-received request after ~1 s */
#define FF_LINK_OUTAGE_MS      300000u   /* a silent link is waited out this long, per request */
#define FF_LINK_OUTAGE_WAIT_MS  10000u
#define FF_KEEPALIVE_MS          5000u   /* NC Flash gives up after 30 s without a line (IDLE_TIMEOUT_S) */
#define FF_PROBE_COUNT              3u   /* 1 KB link probes to the running kernel before the first erase */
#define FF_PROBE_TRIES              2u   /* attempts per probe */
#define FF_PROBE_SUBFN           0xFFu   /* "31 FF" + 1022 bytes: the kernel refuses it with 7F 31 12, harmless */
#define FF_PROBE_LEN             1024u   /* the same request size as a compressed piece */
#define FF_SLICE_MS              1000u   /* longest single wait between two keepalive checks */

/* ---- result codes of one exchange ------------------------------------------------------------ */
enum {
    FF_R_OK    = 0,
    FF_R_NRC   = -2,   /* the ECU refused (ff->nrc) */
    FF_R_LINK  = -3,   /* no answer, or the request could not be sent (ff->sub) */
    FF_R_FATAL = -4,   /* an answer that makes no sense (FlashError in the reference) */
    FF_R_ABORT = -5,   /* the PC went away before the point of no return */
    FF_R_HALT  = -6,   /* the I/O layer says stop now (test harness: simulated dongle power loss) */
};

/* ops->send / ops->recv return codes */
#define FF_IO_OK        0
#define FF_IO_TIMEOUT  (-1)
#define FF_IO_HALT     (-99)
/* any other negative value from send = a transport sub-code (0xE1..0xE4), reported in nrc= */

/* Local sub-codes in the nrc= field of FFERR (the same family as the fast write's FWSUB_*). */
#define FF_SUB_SEND     0xE1   /* first frame could not be sent */
#define FF_SUB_FC_TO    0xE2   /* no flow control */
#define FF_SUB_FC_BAD   0xE3   /* flow control said overflow */
#define FF_SUB_CF_SEND  0xE4   /* a consecutive frame could not be sent */
#define FF_SUB_NO_ANS   0xE5   /* no answer */
#define FF_SUB_BAD_ANS  0xE8   /* an answer of the wrong shape */
#define FF_SUB_MISMATCH 0xE9   /* a CRC that does not match */

/* ---- FFERR stages (docs/internals/fast_flash.md lists them for the PC) ------------------------ */
enum {
    /* the ECU was not contacted */
    FF_ST_NO_FILE = 1, FF_ST_MANIFEST = 2, FF_ST_RULES = 3, FF_ST_SIZE = 4, FF_ST_CRC = 5,
    FF_ST_NOMEM = 6, FF_ST_LIVE_GATE = 7, FF_ST_COMMAND = 8, FF_ST_BUSY = 9, FF_ST_FENCE = 14,
    FF_ST_HOST_GONE = 15,
    /* the kernel phase: the ROM is untouched */
    FF_ST_DOWNLOAD = 20, FF_ST_UPLOAD = 21, FF_ST_UPLOAD_LOST = 22, FF_ST_IDENTIFY = 23,
    FF_ST_KVERSION = 24, FF_ST_KCRC = 25,
    FF_ST_KERNEL_RUNNING = 26,   /* the kernel RequestDownload was refused because our kernel already runs: use R */
    /* the compare phase: the ROM is untouched */
    FF_ST_COMPARE = 30, FF_ST_BOOT = 31, FF_ST_NOT_READY = 32,
    FF_ST_PROBE = 33,            /* the 1 KB link probe to the running kernel failed */
    FF_ST_ERASE15_REFUSED = 34,  /* the first block-15 erase was refused outright: nothing was erased */
    /* past the point of no return: the ECU waits in its boot loader */
    FF_ST_ERASE = 40, FF_ST_OPEN = 41, FF_ST_PIECE = 42, FF_ST_CLOSE = 43,
    FF_ST_VERIFY_LINK = 50, FF_ST_VERIFY = 51,
    FF_ST_COMMIT = 60, FF_ST_COMMIT_LINK = 61,
    FF_ST_COMMIT_STUCK = 62,     /* commit page wrong AND block 15 could not be erased again: send R, do not
                                    cycle the ignition (the ECU may hold a programmed entry pointer) */
};

/* ---- manifest ------------------------------------------------------------------------------- */
typedef struct { uint32_t addr, len, crc; } ff_cmp_t;
typedef struct { uint32_t addr, size, off, len, crc; } ff_range_t;

typedef struct {
    uint8_t    ncmp, npre, nr;
    ff_cmp_t   cmp[2];   /* compare ranges, commit page included */
    ff_cmp_t   pre[2];   /* compare ranges as they must read just before the commit */
    ff_range_t r[2];     /* compressed program ranges and where their stream sits in the package */
} ff_block_t;

typedef struct {
    uint32_t   pkg_len, pkg_crc;
    uint32_t   k_off, k_len, k_crc, k_ver;
    uint32_t   lp, pb, piece;
    ff_block_t blk[16];  /* indices 2..15 */
    uint32_t   boot_crc;
    bool       boot_allow;
    uint32_t   c_off, c_len, c_crc;
    bool       rewrite_all;
} ff_manifest_t;

/* ---- I/O ------------------------------------------------------------------------------------ */
typedef struct ff_status {
    char     mode;          /* 'D' 'C' 'L' 'R' */
    char     phase[12];     /* load kernel compare write verify commit reset done failed */
    int      blk;           /* block being worked on, 0 if none */
    uint32_t done, total;   /* blocks rewritten / to rewrite */
    bool     ponr;
} ff_status_t;

typedef struct ff_ops {
    void *ctx;
    /* Send one ISO-TP request (single or multi frame) to the ECU. FF_IO_OK, FF_IO_HALT, or a
       negative transport sub-code (-FF_SUB_*). */
    int (*send)(void *ctx, const uint8_t *req, uint32_t len);
    /* Wait up to timeout_ms for the next single-frame answer from the ECU; copy its payload (PCI
       stripped). FF_IO_OK, FF_IO_TIMEOUT or FF_IO_HALT. */
    int (*recv)(void *ctx, uint8_t *buf, uint32_t cap, uint32_t *len, uint32_t timeout_ms);
    /* Drop every answer that has already arrived (a late one must never be read as the next). */
    void (*flush)(void *ctx);
    uint32_t (*now_ms)(void *ctx);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    /* One status line to the PC. Return 0 when it was queued, nonzero when the PC is gone. Past the
       point of no return (ponr) the implementation must never block on it. */
    int (*emit)(void *ctx, const char *line, bool ponr);
    /* Journal the point of no return (best effort, never fatal). May be NULL. */
    void (*persist)(void *ctx, const char *event);
    /* Status changed (phase, block, ponr). May be NULL. */
    void (*on_status)(void *ctx, const ff_status_t *st);
} ff_ops_t;

/* ---- one operation -------------------------------------------------------------------------- */
typedef struct ff {
    ff_ops_t       ops;
    ff_manifest_t  m;
    const uint8_t *pkg;
    uint32_t       pkg_len;
    bool           checked;          /* ff_check() passed for this pkg/manifest */

    ff_status_t    st;
    bool           allow_live;       /* the NCFF_ALLOW_LIVE compile gate: L and R refused when false */
    bool           host_gone;        /* an emit failed before the point of no return */
    bool           force_io;         /* the closing 11 01 goes out even when the PC is gone */
    int            stage_hint;       /* FFERR stage of the request in flight */
    uint32_t       cur_addr;         /* address of the request in flight, for FFERR a= */
    bool           kernel_running;   /* identify answered: an 11 01 will restart the application */
    bool           kernel_preexisting; /* the kernel was already running before this op (mode R, or st 26) */
    uint32_t       last_line_ms;

    /* last exchange */
    uint8_t        rsp[8];
    uint32_t       rsp_len;
    uint8_t        nrc;              /* NRC of the last refusal */
    uint8_t        sub;              /* FF_SUB_* of the last link error */

    /* outcome */
    int            err_stage;        /* 0 = no error */
    uint32_t       err_addr;
    uint8_t        err_nrc;
    int            err_blk;
    bool           err_ponr;
    bool           ok;
    bool           nothing_to_do;
    bool           halted;

    uint8_t        kver, kcls, kready;
    bool           resumed;
    bool           boot_same, cut_short;
    bool           entry_known;      /* the entry pointer was read (cut_short is meaningful) */
    bool           probe_ok;
    uint32_t       probe_lost;       /* probe attempts without the expected answer */
    int            stale_seq;        /* -1, or a "76 seq" answer to ignore (a duplicate of the last piece) */
    uint16_t       diff_mask;        /* bit b = block b differed before */
    uint16_t       rewrite_mask;     /* bit b = block b was rewritten */
    uint32_t       crc_before[16][2], crc_after[16][2];
    uint32_t       bytes_sent, resends, reerases, restarts, foreign, outages, pendings;
    uint32_t       t0_ms, ms_kernel, ms_compare, ms_write, ms_verify, ms_commit, ms_total;

    char           result_json[900]; /* the NCFFRES payload */
    char           crc_json[1400];   /* before/after CRC table, for GET /fast_flash */

    uint8_t        req[1 + 1 + FF_KERNEL_BLOCK + 8];
    char           line[1000];       /* one status line (kept off the 6 KB can_tx_task stack) */
} ff_t;

/* zlib CRC-32 (same as Python zlib.crc32): pass 0 to start, the previous result to continue. */
uint32_t ff_crc32(uint32_t crc, const uint8_t *data, size_t len);

/* zlib crc32_combine(): CRC of A followed by B, from crc(A), crc(B) and the length of B. */
uint32_t ff_crc32_combine(uint32_t crc1, uint32_t crc2, uint32_t len2);

/* 'u' unknown, '1' the ECU's entry pointer is programmed, '0' it reads erased (no application). */
char ff_app_state(const ff_t *ff);

/* Start range of an erase block; FF_ROM_SIZE for block 16. */
uint32_t ff_block_start(unsigned blk);

/* Parse a manifest. 0 or an FF_ST_* stage (MANIFEST for bad JSON, RULES for a schema violation),
   with *bad_addr naming the offending address or block where there is one. */
int ff_manifest_parse(const char *json, size_t len, ff_manifest_t *m, uint32_t *bad_addr);

/* The hard rules on a parsed manifest (ranges, kernel image, commit page) against the package, plus
   every CRC. 0 or an FF_ST_* stage. Called by ff_check; exposed for the tests. */
int ff_manifest_validate(const ff_manifest_t *m, const uint8_t *pkg, uint32_t pkg_len, uint32_t *bad_addr);

/* Initialise an operation. mode: 'D' dry run (no CAN), 'C' compare only (kernel upload + CRCs, no
   erase), 'L' live, 'R' resume live with the kernel already running. allow_live = the compile gate. */
void ff_init(ff_t *ff, const ff_ops_t *ops, char mode, bool allow_live);

/* Parse and validate manifest + package (everything before any ECU contact). Emits FFERR and
   returns its stage on failure, 0 on success. pkg must stay valid until ff_run returns. */
int ff_check(ff_t *ff, const char *json, size_t json_len, const uint8_t *pkg, uint32_t pkg_len);

/* Run the operation (mode set at ff_init) on a checked package. 0 on success (NCFFDONE emitted),
   else the FFERR stage (FFERR emitted), or -1 when halted by the I/O layer. 'D' does no CAN. */
int ff_run(ff_t *ff);

/* Emit the FFERR line for a failure found outside ff_run (busy, fence, bad command). */
void ff_fail_early(ff_t *ff, int stage, uint32_t addr);

#endif /* FAST_FLASH_LOGIC_H */
