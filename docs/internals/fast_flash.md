# Fast flash (`K`): the dongle rewrites only the ECU blocks that differ

The PC uploads a package and its manifest to the SD card, then sends one line. The dongle uploads
the nc-flash-re fast-flash kernel (v3) to the ECU, asks it for the CRC of every flash block, and
rewrites only the blocks that differ, plus block 15. It verifies everything, then writes the commit
page that makes the ECU bootable again. A one-block change took 3.9 s over a Tactrix on the bench
(PC reference, 2026-11-15). The firmware port is **not bench-tested yet** (see the checklist at the
end).

| Piece | Where |
|---|---|
| Decision logic, IDF-free: sequence, retry rules, manifest rules | `fast_flash_logic.c`, `fast_flash_logic.h` |
| ESP side: ISO-TP over TWAI, PSRAM load, bus ownership, `GET /fast_flash` | `ncflash_fastflash.c`, `ncflash_fastflash.h` |
| Trigger dispatch | `can_tx_task` in `main.c`, next to the `W` fast write |
| Reference packer (NC Flash ports it) | `tools/fast_flash_package.py` |
| Host tests: fake kernel over a 1 MB model flash | `tools/hosttest/fast_flash_test.c`, `fast_flash_fake_ecu.c`, `fast_flash_gen.py` |

The logic mirrors the PC reference implementation (nc-rom-editor src/ecu/fast_flash.py, frozen
2026-11-15) function by function; the header of `fast_flash_logic.h` maps each one. The kernel's
own protocol is the header comment of the kernel source in nc-flash-re
(tools/fast_flash/kernel/kernel.c).

## Safety model

- **Compile gate.** `NCFF_ALLOW_LIVE` is 1 by default since v1.28.0-beta.1 (bench test passed
  2026-11-16; NC Flash sends `L`/`R` only when the user turned the block compare on, off by
  default). A build with `idf.py -DNCFF_ALLOW_LIVE=0` (passed through by `main/CMakeLists.txt`)
  runs only `D` and `C`; `L` and `R` then end in `FFERR st=7` before any CAN traffic.
  `/host_caps` reports the gate as `ff_live`.
- **Before any ECU contact** the whole package is read into PSRAM and `ff_check()` runs
  `ff_manifest_parse()` and `ff_manifest_validate()`:
  - the whole-package CRC and the CRC of every section;
  - every program range must be exactly the range the firmware derives itself from the block map
    (`_lz_ranges` in the reference), and must also pass the hard rule on its own: page aligned,
    at or above 0x2000, ending at or before 0xFFF80, clear of the counter page 0xFFB00-0xFFB7F;
  - every compare range must be exactly the derived one (`compare_ranges`); all 14 blocks 2..15
    present once;
  - kernel: whole 0x400 blocks up to 0x3000, `*SBL_END` in the last block only, version >= 3;
  - commit page: at 0xFFF80, 128 bytes, entry pointer (last 4 bytes) not 0xFFFFFFFF;
  - the compare CRCs and the commit page describe ONE image: `cmp_pre` equals `cmp` for blocks
    2..14 and for block 15's first range, and block 15's second `cmp` CRC is its `cmp_pre` CRC
    continued over the package's commit page (a packer that mixed two images is refused);
  - lz settings lp, pb <= 2, piece 1..1022 bytes.
  The same validation runs again just before the first erase (catches a PSRAM change). The request
  builders refuse on their own to erase outside blocks 2..15 or to open a range that breaks the
  rule, whatever the manifest says. The SD card is not read after the first ECU request.
- **Kernel checks before the first erase:** identify must answer version == manifest version, and
  the kernel's CRC of its own RAM image (`31 03`) must equal the package's kernel CRC. A kernel
  that arrived damaged never gets an erase.
- **Link probe before the first erase (modes C, L, R):** three full-size requests to the running
  kernel, `31 FF` + 1022 bytes, each answered `7F 31 12` (the kernel's routine service refuses the
  unknown sub-function; nothing changes). A compressed piece is the same 1024-byte, 147-frame
  burst into the kernel's single receive mailbox, so this proves before anything is erased that
  every piece can get through. Two tries per probe; a failure is `FFERR st=33` with the ROM
  untouched. Never a bare `36` as a probe: in mode R a compressed range may still be open and the
  kernel would decode it into flash. The kernel upload's own 1 KB blocks prove nothing here: they
  go to the ROM loader.
- **Boot block:** the ECU's CRC of 0x0000-0x1FFF must equal `boot.crc32`, or the flash is refused
  (`FFERR st=31`) unless the manifest says `"policy":"allow"`. NC1 and NC2 boot blocks differ, so
  this is a second wrong-generation guard. Compare-only reports it without refusing.
- **Point of no return** = just before the block-15 erase. Before it, a PC that stops taking lines
  for 2 s aborts the op (`st=15`) and the ECU gets `11 01` back to its untouched application. From
  it on, lines are 0-tick tries whose loss is only counted: nothing the PC or WiFi does stops the
  flash. Block 15 is erased FIRST, so from then until the commit page verifies, the boot loader
  does not start the application, and a cut at any point leaves the ECU in its loader.
  Exception: if that very first block-15 erase is refused outright (an NRC other than `72`, e.g.
  `22` for a flash counter of 100 or more), nothing was erased: `FFERR st=34 ponr=0`, `11 01`.
- **An ECU found without an application** (entry pointer reads erased, `cut=1`) is never sent
  `11 01` by a failing op: the kernel stays up so `R` can rewrite it. The op journals it (see
  `GET /fast_flash`) and `FFERR` says `app=0`.
- **Commit last:** the commit page goes out only after every block was read back by CRC (commit
  page excluded) and matched, and after one more CRC over 0x2000-0xFFB00 matched the value
  combined from the manifest (a stale CRC answer cannot fake both). If the page then reads wrong,
  or the kernel refused its write with `72` (its own read-back failed: marginal cells, even if the
  CRC matches; the PC reference trusts the CRC here), block 15 is erased again (`st=60`). If that
  re-erase fails too, `st=62`: the ECU may hold a programmed entry pointer on a wrong page, the
  kernel still runs: send `R` and do NOT cycle the ignition first. A lost answer never erases: the
  CRC decides (review 2026-11-15 finding 3).
- **After a failure past the point of no return no `11 01` is sent:** the kernel keeps running so
  mode `R` can finish. The kernel has no idle timeout.
- **Bus ownership** exactly as the fast write: `can_flash_active_set()` (parks the datalogger,
  refuses SLCAN/MQTT producers, sleep, reboot, OTA, config saves and SD uploads), the reboot/OTA
  fence `flash_fence_write_may_start()`, `can_rx_task` suspended; released last on every exit.
  The `W` fast write's code (`ncflash_fastwrite.c`) is not touched.

## Trigger

On the dedicated SLCAN port 35001, one line: `K` + mode + package leaf name + CR, e.g.
`KLLF9VEB_tune.ffp\r`. The manifest is `/sdcard/roms/<stem>.json` next to
`/sdcard/roms/<stem>.ffp`. Both go up with `POST /upload/sd/<name>` first (check the returned
`crc32`). Never send `K` unless `/host_caps` has `fast_flash`: older firmware hands it to the SLCAN
parser.

| Mode | What it does | Needs |
|---|---|---|
| `D` | load + check the package; no CAN | |
| `C` | kernel upload, identify, self CRC, CRC of every block, boot block, entry pointer; then `11 01`. Nothing erased | the PC's normal flash session first |
| `L` | live: only differing blocks + block 15, verify, commit, `11 01` | `ff_live` = 1 and the PC's normal flash session first (programming session, security access, generation check, flash counter) |
| `R` | resume: probe the running kernel (no upload, no auth), then as `L`; the entry pointer reads erased, so every block is rewritten | `ff_live` = 1, a kernel still running |

`L` against an ECU whose fast-flash kernel is already running (an earlier op was cut short) gets
its RequestDownload refused by that kernel; the firmware then probes identify and answers
`FFERR st=26`: send `R`.

## Stream lines (newline-terminated ASCII on 35001)

| Line | Fields |
|---|---|
| `NCFFSYNC` | the bus is taken (after busy/fence checks, before the package checks) |
| `NCFFSTEP <phase>` | `kernel`, `compare`, `write`, `verify`, `commit`, `reset` |
| `NCFFKERN v= cls= ready= resumed=` | identify answer |
| `NCFFPROBE ok=0/1 lost=N` | the 1 KB link probe; `lost` = probe tries without the kernel's answer |
| `NCFFCMP diff=<blocks> boot=0/1 cut=0/1 rewrite=<blocks>` | blocks as `3,9,15` or `-`; `cut=1` = entry pointer erased (earlier flash cut short, everything is rewritten); `rewrite` is `-` in mode `C` |
| `NCFFPROG <done>/<total> blk=<b>` | at each block start, and as the keepalive at least every 5 s whatever the ECU does |
| `NCFFNOTE <what> ...` | `resend kernel a=`, `reerase blk= nrc=`, `upload_restart n=`, `outage sid= a=`, `landed a=`, `commit_refused nrc=`, `commit_no_answer`, `reset ok/no_answer` |
| `NCFFRES {json}` | the result object below; sent before `NCFFDONE` or `FFERR` |
| `NCFFDONE mode=X nothing=0/1 rewritten=N` | terminal success |
| `FFERR a=<hex6> st=<n> nrc=<hex2> blk=<b> ponr=0/1 app=0/1/u` | terminal failure, framed `\r\n...\r\n` |

**`ponr` says whether the ROM may be touched; `app` says whether the ECU has an application.**
- `ponr=0`: this op touched nothing. If the kernel had started and the ECU has an application
  (`app=1`), it got `11 01` and the ECU restarts it (re-authenticate before anything else). With
  `app=0` the ECU had no application when this op found it: the kernel was kept up, send `R`.
- `ponr=1`: past this op's point of no return, or a kernel was already running from an earlier op
  that may have been cut after its own (mode `R`, `st=26`). The kernel is kept up: send `R` while
  the ECU stays powered; if it was power-cycled, a full flash (`W`, or the PC's own) recovers it.
- `app=1` entry pointer read programmed; `app=0` read erased (no application); `app=u` not known
  (not read by this op, or `st=61`/`62`). `R` failing with `st=23` (no kernel) says `app=u`: if
  `GET /fast_flash` said `interrupted`, assume no application.

`nrc` is the ECU's NRC, or a local code: `E1` first frame not sent, `E2` no flow control, `E3`
flow control overflow, `E4` consecutive frame not sent, `E5` no answer, `E8` an answer of the wrong
shape, `E9` a CRC that does not match.

### FFERR stages

| st | Meaning | ROM |
|---|---|---|
| 1 | SD not mounted / package missing or unreadable | not contacted |
| 2 | manifest missing, over 8 KB, or not valid JSON (integers only) | not contacted |
| 3 | a manifest rule is broken (`a=` names the address or block) | not contacted |
| 4 | package size differs from `package_len`, or over 2 MB | not contacted |
| 5 | a CRC of the package or of one section does not match | not contacted |
| 6 | out of memory (PSRAM) | not contacted |
| 7 | `L`/`R` on a build without `NCFF_ALLOW_LIVE` | not contacted |
| 8 | bad command (mode, name) | not contacted |
| 9 | another fast op owns the bus | not contacted |
| 14 | a reboot or firmware update is pending | not contacted |
| 15 | the PC stopped taking lines before the point of no return | untouched, `11 01` sent |
| 20 | RequestDownload of the kernel refused / unanswered | untouched |
| 21 | a kernel block refused / unanswered | untouched |
| 22 | the upload lost its place and could not restart | untouched |
| 23 | identify failed; in mode `R`: no kernel running | untouched |
| 24 | kernel version differs from the manifest (or < 3) | untouched, `11 01` sent |
| 25 | kernel self CRC does not match the package | untouched, `11 01` sent |
| 26 | `L`, but our kernel already runs (left by an op that was cut): use `R` | as that earlier op left it (`ponr=1`), kernel kept |
| 30 | a CRC request failed during the compare | untouched, `11 01` sent |
| 31 | boot block differs and the policy is `refuse` | untouched, `11 01` sent |
| 32 | the kernel could not prepare the flash routines (`ready=0`) | untouched, `11 01` sent |
| 33 | the 1 KB link probe to the kernel failed | untouched, `11 01` sent |
| 34 | the first block-15 erase was refused outright (flash counter >= 100, flash not ready) | untouched, `11 01` sent (follows `NCFFSTEP write`) |
| 40 | an erase failed | **boot loader** |
| 41 / 42 / 43 | opening a range / a piece (refused twice, or unanswered for 5 min) / closing a range | **boot loader** |
| 50 | the verify CRCs could not be read | **boot loader** |
| 51 | a block did not verify (`blk=` the first bad one) | **boot loader** |
| 60 | the commit page did not verify; block 15 was erased again | **boot loader** |
| 61 | the link was lost while checking the commit page; every block had verified | unknown: may be complete, flash again to confirm |
| 62 | the commit page reads wrong AND block 15 could not be erased again | **may hold a wrong entry pointer**: send `R` before any ignition cycle |

For stages 1-32 "untouched, `11 01` sent" holds when the ECU had an application; with `app=0` the
kernel is kept instead.

## Manifest (`<stem>.json`, at most 8 KB, integers only)

```
{"kind":"nc_fast_flash","manifest_version":1,"package_len":N,"package_crc32":C,
 "kernel":{"off":0,"len":6144,"crc32":C,"version":3},
 "lz":{"lp":2,"pb":2,"piece":1022},
 "blocks":[
   {"block":2,"cmp":[[8192,4096,C]],"ranges":[{"addr":8192,"size":4096,"off":O,"len":L,"crc32":C}]},
   ...
   {"block":15,"cmp":[[917504,129792,C],[1047424,1152,C]],
               "cmp_pre":[[917504,129792,C],[1047424,1024,C]],
               "ranges":[{"addr":917504,"size":129792,...},{"addr":1047424,"size":1024,...}]}],
 "boot":{"crc32":C,"policy":"refuse"},
 "commit":{"addr":1048448,"off":O,"len":128,"crc32":C},
 "rewrite_all":false,"cal_id":"...","generation":"NC2"}
```

- `cmp`: the CRC the ECU must report for each compare range of the block in the image (commit page
  included). `cmp_pre`: the same just before the commit (commit page excluded); required for
  block 15, defaults to `cmp` for the others.
- `ranges[].crc32` is the CRC of the stream bytes in the package; the ECU-side check of the
  decompressed data is the verify step.
- Unknown keys are ignored (`cal_id`, `generation` are for people).
- The package is the kernel, then one raw LZMA1 stream per range, then the commit page; the order
  is free, the offsets say where. `tools/fast_flash_package.py` is the reference.

## `GET /fast_flash`

```
{"rev":1,"live_allowed":0,"state":"idle|running|done|failed|interrupted","name":"x.ffp","mode":"L",
 "phase":"write","blk":9,"done":1,"total":3,"ponr":1,"elapsed_ms":12345,"lines_dropped":0,
 "err":{"st":0,"nrc":0,"a":0,"blk":0,"ponr":0},"result":{...}|null,"crc":{...}|null}
```

- `state` `interrupted` (after a dongle restart): the ECU may have no application. The journal
  `/sdcard/roms/ff_journal.json` is written just before the block-15 erase, and as soon as an `L`
  or `R` finds the entry pointer erased. It is removed only by an op that succeeds, or by an `L`
  that fails with `ponr=0` and `app` not `0`; never by a failing `R`. Send `R` (or a full flash if
  the ECU was power-cycled meanwhile).
- `stack_free`: `can_tx_task`'s stack high-water mark after the last op, in bytes (the device has
  no serial console).
- `result` (also the `NCFFRES` payload): `mode ok nothing kv cls ready resumed boot_same cut diff[]
  rewritten[] bytes resends reerases restarts outages foreign pendings ms{kernel compare write
  verify commit total} probe{ok lost} app err{st a nrc blk ponr}`.
- `crc`: `{"before":[[blk,crc1,crc2],...],"after":[...]}` as the ECU reported them (0 = not read).
- Also written to `/sdcard/roms/<stem>.result.json` after the op.
- Works during a flash (it is HTTP, not the one-client port 35001).

## `GET /host_caps`

`"fast_flash":1` (`NCFLASH_FASTFLASH_REV`, the wire + manifest version) and `"ff_live":0|1`. Absent
on older firmware: fall back to the SD-staged full flash.

## Retry rules (mirrored from the reference)

- Every request first drops answers that already arrived (a late answer is never read as the next
  one). A negative answer naming another service, or a positive answer to another service, is
  another tester's: counted (`foreign`) and ignored, as is a `76` repeating the previous piece's
  seq. `7F <sid> 78` extends the wait.
- Idempotent requests (CRC, erase, identify, open range, kernel self CRC, compressed pieces): 4
  quick retries 1.5 s apart, then up to 5 min of 10 s retries (`FF_LINK_OUTAGE_MS`); a probe
  (`R`'s identify, the upload's out-of-step probe) gets one retry.
- Kernel upload: a block without an answer is sent again; if the loader refuses the resend, it had
  the block: probe identify (the kernel runs if it was the last block), otherwise restart from
  RequestDownload, at most 2 times (`FF_UPLOAD_RESTARTS`).
- Compressed piece `36 seq data`: a lost answer means the same seq is sent again (the kernel
  answers it again without using it twice). A refusal: erase the block again and resend the whole
  block once; a second refusal is `st=42`.
- TransferExit: if its answer was lost and the resend is refused `24`, the range was closed.
- Commit page: written plain; a lost answer re-opens the range (refused `22`, the page went in);
  the CRC decides.
- Timeouts: erase 30 s, CRC 10 s, piece 6 s (pending 30 s), plain write 4 s, identify 2 s; no
  request waits longer than 5 min in total.

## Around the op

- Lines the PC sends on 35001 while the op runs are dropped when it returns (they queue behind it
  and would otherwise hit an ECU that was just reset).
- The dead-man reaper's stuck-flash alarm waits `NCFF_STUCK_FLASH_CEILING_US` (30 min) instead of
  `COEXIST_STUCK_FLASH_CEILING_US` (180 s) while a `K` op runs: one riding out a link outage
  legitimately holds the bus for minutes. The alarm never clears the bus either way.
- A build directory configured with `-DNCFF_ALLOW_LIVE=1` keeps it in its CMake cache: never OTA a
  car's WiCAN from such a directory; check `ff_live` in `/host_caps` after any OTA.

## Bench checklist (human, before `NCFF_ALLOW_LIVE` ships)

Take `~/.claude/wican-bench.lock` and snapshot device state first.

1. `D` on the live-gate build: `NCFFDONE mode=D`, no CAN frames.
2. `C` against the bench ECU after the PC's normal session: `NCFFPROBE ok=1 lost=0`, the CRC table
   matches the Tactrix compare-only run, the ECU runs its application again after `11 01`. The
   probe is the proof that the ESP's back-to-back 1 KB bursts reach the running kernel (its single
   mailbox) before anything is erased; repeat `C` a few times and note `lost`. If `lost` is not 0,
   set `NCFF_CF_GAP_US` before any `L`. Read `stack_free` from `GET /fast_flash`.
3. `L` with a one-block change: compare the time with the Tactrix's 3.9 s; read back after an
   ignition cycle.
4. `L` with the same image: `nothing=1`. Then `rewrite_all`.
5. Kill NC Flash and the WiFi mid-flash: it finishes; `GET /fast_flash` reports the result.
6. Pull the dongle's power after the point of no return; power it, `GET /fast_flash` says
   `interrupted`; send `L` once (expect `st=26`, "use R"), then `R`.
7. ECU power cut mid-flash: recover with the full SD flash; try `C`/`L` from the recovery server.
8. A corrupt-stream package: re-erase, `FFERR`, recovery.
9. Old NC Flash with this firmware; new NC Flash with old firmware (no `fast_flash` key).
10. Sleep, OTA and reboot refused during the flash; reset count unchanged.
11. A run that rides out a long link outage (WiFi off is not enough: unplug the ECU's CAN for
    ~1 min after the point of no return): no `NCSTUCKFLASH` before 30 min, the flash completes.
12. Send a few SLCAN lines on 35001 during an op: they are dropped (warning in the log), not run
    after it.
