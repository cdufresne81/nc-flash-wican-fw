/*
 * NC Flash FAST FLASH -- the dongle rewrites only the ECU blocks that differ, by itself.
 *
 * The PC uploads two files with POST /upload/sd/<name>: a package <stem>.ffp (the nc-flash-re
 * fast-flash kernel, one raw LZMA1 stream per program range, the 128-byte commit page) and its
 * manifest <stem>.json. Then, on the dedicated SLCAN port 35001, it sends ONE line:
 *
 *     'K' + mode + "<stem>.ffp" + CR          e.g. "KL LF9VEB_tune.ffp\r" without the space
 *
 *   mode 'D' dry run: load + check the package, no CAN at all
 *        'C' compare only: kernel upload, CRC of every block, then 11 01 (nothing erased)
 *        'L' live (needs NCFF_ALLOW_LIVE=1): only differing blocks + block 15, verified, committed
 *        'R' resume live: the kernel is already running (an earlier op was cut after its point of
 *            no return); no upload, every block rewritten
 *
 * The PC must have done its usual session (programming session, security access, generation
 * check, flash counter) before 'C' or 'L', exactly as before the SD-staged fast write ('W'). 'R'
 * needs none: the kernel refuses 10/27 anyway.
 *
 * The whole package is loaded into PSRAM and CRC-checked before any ECU contact; the SD card is not
 * read after that. From just before the block-15 erase (the point of no return) the flash finishes
 * alone: nothing the PC or WiFi does can stop it.
 *
 * Wire contract (lines, manifest, FFERR stages, GET /fast_flash): docs/internals/fast_flash.md.
 * Decision logic, IDF-free and host-tested: fast_flash_logic.c.
 */
#ifndef NCFLASH_FASTFLASH_H
#define NCFLASH_FASTFLASH_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_http_server.h"

#define NCFLASH_FASTFLASH_CMD 'K'
#define NCFLASH_FASTFLASH_MIN_LEN 3   /* 'K' + mode + at least one name character */

/* Wire + manifest version reported by GET /host_caps as "fast_flash". */
#define NCFLASH_FASTFLASH_REV 1

/* HARD SAFETY GATE. 0 = only 'D' and 'C' (nothing can be erased); 1 = 'L' and 'R' allowed.
 * Set from the build: idf.py -DNCFF_ALLOW_LIVE=1 (main/CMakeLists.txt passes it through).
 * Keep 0 until the bench steps in docs/internals/fast_flash.md have passed. */
#ifndef NCFF_ALLOW_LIVE
#define NCFF_ALLOW_LIVE 0
#endif

int ncflash_is_fastflash_cmd(const uint8_t *buf, int len);

/* Run one fast-flash command inline (can_tx_task), streaming lines to *tx_queue.
 * 0 = NCFFDONE; nonzero = FFERR stage (already streamed). */
int ncflash_fast_flash(const uint8_t *buf, int len, QueueHandle_t *tx_queue);

/* True while a 'K' op runs. The dead-man reaper uses a longer stuck-flash ceiling for it: a fast
 * flash that rides out a link outage legitimately holds the bus for many minutes. */
bool ncflash_fastflash_active(void);

/* The stuck-flash alarm ceiling while a 'K' op runs (COEXIST_STUCK_FLASH_CEILING_US otherwise). Every
 * request of the op is bounded (FF_REQ_CEILING_MS, the 5 min outage wait per request), so a fast
 * flash that is still making progress stays below this; one past it is wedged. */
#define NCFF_STUCK_FLASH_CEILING_US (30ULL * 60ULL * 1000000ULL)

/* GET /fast_flash. Returns the number of handlers registered. */
int ncflash_fastflash_register_handlers(httpd_handle_t server);

#endif /* NCFLASH_FASTFLASH_H */
