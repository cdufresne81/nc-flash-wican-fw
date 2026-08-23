# Design brief: record what the host is doing on the bus (issue #122)

**Status: PRE-DESIGN.** This document captures the requirement and everything already verified
against the source, so a later session does not have to rediscover it. It deliberately does
**not** carry a `/goal` condition yet — the architecture has to be designed first (see §6).
Once that design exists, this file gets its design section, its acceptance criteria and its
`/goal` line, and becomes a normal goal document.

---

## 1. The goal

Make the device's own event log say **what** a connected host did on the CAN bus, not merely
that a host held it. Today an entire ROM read or a failed flash attempt can sit inside a single
`HOST_CLAIM` … `HOST_RELEASE` pair with nothing in between.

The log has to stand on its own, because it is the only record that survives the host
application closing, crashing, or being on a laptop that has since gone home.

## 2. What triggered this

A real session on 2026-08-23, reconstructed after a flash failed at SBL block 5 of 6:

```
09:31:13 HOST_CLAIM   host claimed bus
   ... 6 minutes with no entries at all ...
09:37:59 FLASH_START  v23_LFZZEA_sparkdown_20260823_0937.bin LIVE blocks=1022
09:38:14 FLASH_FAIL   LIVE rc=-2 st=11 nrc=0xE5 blk=5/1022 name=...
09:38:14 HOST_RELEASE host released bus
```

Those six silent minutes were a full 1 MB ROM read. The device logged none of it. The fact that
mattered — *"16 block(s) re-requested after a dropped frame on the lossy link"* — existed only
in the host's own log file. Had the host log been lost, the device would have offered no hint
that the link was already dropping frames minutes before the flash was started on it.

Owner's sketch of the wanted output (wording not final):

```
HOST_CLAIM   bus claimed by host (NC Flash)
NC_FLASH     Read DTCs
NC_FLASH     Clear DTCs
NC_FLASH     Read RAM
NC_FLASH     Read ROM
NC_FLASH     Write full ROM (<file>)
NC_FLASH     Write partial ROM (<file>)
HOST_RELEASE bus released by host (NC Flash)
```

## 3. Why the firmware cannot do this alone — verified

- The two lines that already exist are emitted by the firmware because the firmware itself
  drives that operation: `event_log_emit(EVL_FLASH_START, ...)` at
  `main/ncflash_fastwrite.c:453-456` and `EVL_FLASH_FAIL` at `main/ncflash_fastwrite.c:562`.
  This is the SD-staged fast-write path, which runs entirely on the device.
- **Every other operation is raw SLCAN.** DTC read, DTC clear, RAM read, ROM read and mode
  changes all run over the dedicated port 35001 as plain CAN frames. The firmware sees frames,
  not intent, and cannot label them without guessing at UDS service IDs — which would be a
  fragile parser sitting in the middle of a path that must never disturb an ECU write.
- Therefore the host must **declare** what it is starting and how it ended. This is a
  cross-repo change: `nc-flash-wican-fw` (firmware, the log) and `nc-rom-editor` (NC Flash,
  the caller).

## 4. Where it would attach

- The coexistence HTTP surface already exists from #92 and is the natural home, alongside
  `/datalog` and `/host_caps`.
- Leases and identity already exist: a host bus claim carries a `claim_token` with a 75 s TTL,
  the datalog park carries a `park_token` with a 12 s TTL, and a dead-man reaper ticks every
  1 s. An annotation can be bound to the same token rather than inventing new identity.
- `/host_caps` reports `ncfr_rev` (currently 6). Any new endpoint is additive, so the revision
  question is whether annotations need to be discoverable or can be fire-and-forget.

## 5. Constraints that must not be broken

- **C1.** Nothing here may add work, latency or a lock to the ECU write path. A flash that is
  in progress outranks any logging.
- **C2.** An old NC Flash that never annotates must behave exactly as it does today — no new
  errors, no missing-annotation warnings, no change to `HOST_CLAIM` / `HOST_RELEASE`.
- **C3.** Host-supplied text reaches `event_log_emit()`, which takes a **format string**
  (`components/event_log/event_log.c:145`). It must never be passed as the format. Length cap
  and character filtering required before it is stored.
- **C4.** The log lives on the SD card. A chatty or looping host must not be able to fill it —
  rate limiting is mandatory, not optional.
- **C5.** The log must not be left asserting that an operation is still running after the host
  has gone. The reaper already detects that; the design has to decide what it writes.

## 6. Open design questions — for Fable

1. Endpoint shape, and whether an annotation is bound to the claim/park token so a stale or
   second host cannot write into another session's log.
2. Start/end pairing. Does the host send both, or one call with a duration? What is written
   when the end never arrives (host crash, cable pulled, app frozen)?
3. Category design: one new `NC_FLASH` category, or reuse of existing ones? Note issue #103 is
   already open on severity being a field rather than a category — these two should not fight.
4. The sanitising and rate-limiting rules for C3 and C4, concretely.
5. Whether the firmware should additionally log what it *can* observe (bus idle time, frames
   retried, link quality) so a host that lies or dies still leaves a usable trace.
6. Whether this bumps `ncfr_rev` or stays purely additive.

## 7. Related work

- Issue #92 — the coexistence surface, leases and reaper this would attach to.
- Issue #103 — event-log severity as a field, not a category; overlaps §6.3.
- Issue #108 — diagnostic bundle; a richer log makes that bundle materially more useful.

## 8. Not in scope here

The pre-flash link-quality gate passed (`loss 0.0%, p95 62 ms`) five seconds before a flash on
a link that had been re-requesting blocks minutes earlier. That gate gap is a genuine safety
issue and belongs in its own issue against `nc-rom-editor`; it is mentioned only so the
connection is not lost.
