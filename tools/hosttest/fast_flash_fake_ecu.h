/* A fake NC ECU for the fast-flash host tests: the ROM loader (kernel upload) and the nc-flash-re
 * fast-flash kernel v3 (nc-flash-re tools/fast_flash/kernel/kernel.c), over a 1 MB model flash,
 * on a virtual clock, with fault injection. It mirrors the kernel's rules, not its timing to the
 * microsecond: the answers and refusals the logic sees are the kernel's.
 *
 * The LZMA decoder is written after the kernel's lz_decode() (itself after LzmaSpec.cpp, Igor
 * Pavlov, public domain), lc = 0, lp/pb <= 2, but made resumable: the kernel pulls pieces from CAN
 * inside the decoder, a fake driven by request/answer calls cannot, so it decodes a symbol only
 * when the input surely holds it, and finishes the tail of a piece on a copy of its state.
 */
#ifndef FAST_FLASH_FAKE_ECU_H
#define FAST_FLASH_FAKE_ECU_H

#include <stdbool.h>
#include <stdint.h>

#define FE_ROM 0x100000u

/* What a request is, for fault targeting (counted per kind, 1-based). */
enum {
    FK_NONE = 0, FK_DL, FK_UPLOAD, FK_IDENT, FK_CRC, FK_ERASE, FK_KCRC, FK_OPEN, FK_OPENLZ, FK_PIECE, FK_PLAIN,
    FK_CLOSE, FK_RESET, FK_TP, FK_PROBE, FK_CRCC, FK_OTHER, FK__COUNT   /* FK_CRCC: CRC of the commit page */
};

enum {
    FA_DROP_ANSWER = 1,   /* the request is processed; its answers are lost */
    FA_DROP_REQUEST,      /* the request never reaches the ECU (send succeeds) */
    FA_FC_FAIL,           /* the request never reaches the ECU and the sender sees no flow control */
    FA_LATE,              /* the answer comes arg ms late */
    FA_REFUSE,            /* a programming failure while this piece/page is programmed (NRC 72) */
    FA_CORRUPT,           /* one payload byte flipped on the way in */
    FA_SLOW,              /* the work takes arg ms more (pendings go out every ~4 s) */
    FA_HALT,              /* the dongle loses power as it sends this request */
    FA_DUP,               /* the answers come twice, the copy arg ms after the original */
    FA_LIE72,             /* plain write: the page is programmed correctly, yet the answer is 7F 36 72 */
};

typedef struct {
    int kind, nth, action;
    uint32_t arg;
    int repeat;            /* also every later request of that kind (persistent fault) */
} fe_fault_t;

typedef struct {
    uint32_t t;
    uint8_t len;
    uint8_t d[8];
} fe_rsp_t;

typedef struct fe_lzd fe_lzd_t;

typedef struct {
    /* configuration */
    uint8_t  kver, kcls, kready;
    bool     loader_restart_ok;
    bool     kernel_running_at_start;   /* resume tests: start inside the kernel */
    bool     app_running;               /* the ECU runs its application: nobody answers 31 00 */
    uint16_t force_counter;             /* nonzero: the flash counter the kernel sees (>= 100 refuses erases) */
    uint32_t erase_ms_small, erase_ms_big, page_us;
    fe_fault_t faults[8];
    int      nfaults;
    uint32_t outage_from, outage_to;    /* ms: frames in this window are lost */
    int      noise;                     /* 1 = a foreign "7F 22 11" and "62 .." before every answer */
    const uint8_t *expect;              /* image the commit must find in flash */
    uint32_t *clock;                    /* the shared virtual clock, ms */

    /* state */
    uint8_t  flash[FE_ROM];
    int      st;                        /* 0 loader, 1 kernel, 2 halted */
    int      dl_active;
    uint32_t klen, khave;
    int      refuse_next_upload;
    uint8_t  kram[0x3000];
    uint8_t  last_erased, flash_ready;
    uint16_t counter;
    uint8_t  prog_open, lz_mode, lz_done, lz_seq, in_decode;
    uint32_t prog_ptr, prog_left, dec_base, pages_done;
    fe_lzd_t *lz;
    uint8_t  *lz_in;                    /* the stream received so far for the open range */
    uint32_t lz_in_len;
    int      cur_kind;
    int      cur_action;
    uint32_t cur_arg;

    fe_rsp_t q[64];
    int      qn;

    /* records */
    int      counts[FK__COUNT];
    int      requests;
    int      erase_count[16];
    int      low_requests;              /* any erase/program request below 0x2000 (must stay 0) */
    int      commit_programmed;
    int      commit_on_good_image;      /* the commit page went in while flash == expect */
    int      programs_after_commit;
    int      resets;
    int      pieces;
    int      probes_before_first_erase; /* -1 until the first erase request */
    int      decode_starved_live;       /* fake self-check: must stay 0 */
} fake_ecu_t;

void fe_init(fake_ecu_t *e, const uint8_t *initial_flash, uint32_t *clock);
void fe_free(fake_ecu_t *e);
void fe_add_fault(fake_ecu_t *e, int kind, int nth, int action, uint32_t arg, int repeat);
/* Start the fake inside a running kernel, as left by an op that was cut short. */
void fe_start_kernel(fake_ecu_t *e);

/* The ops the logic calls (through the test's ff_ops_t). */
int  fe_send(fake_ecu_t *e, const uint8_t *req, uint32_t len);
int  fe_recv(fake_ecu_t *e, uint8_t *buf, uint32_t cap, uint32_t *len, uint32_t timeout_ms);
void fe_flush(fake_ecu_t *e);

#endif
