# Evidence note: NC Flash tool behaviour, and what survived the host-lock design

**Status:** NOT a goal document any more. This is a short evidence note, kept for two reasons only.

1. `docs/goals/remove-slcan-mode.md` cites this file (at :86 and :139) for the NC Flash `slcan_session()` finding. That citation is preserved below.
2. Everything else that was still live moved to **issue #145** (the flash reboot fence and the host-lock overlay design).

Most of this file described the world before v1.23.0 and has been deleted. See the History section for what went and why.

## The cited evidence: how NC Flash switches modes

This is the finding `remove-slcan-mode.md` points at.

**Verified by the implementer against the tool's own source**, `nc-flash src/ecu/wican_config.py`:

- `slcan_session()` calls `set_protocol(SLCAN)` exactly **once** on entry and `restore(true_prev)` exactly **once** on exit.
- `set_protocol` is a **no-op** when the device already reports the target mode.
- There is **no retry loop** around the switch.

So an old tool performs one reboot per switch, two per complete session, and all of its work happens inside a single granted boot.

**Why this still matters after v1.23.0:** it is the basis for saying that NC Flash <= v2.8.0 fails *cleanly* against current firmware rather than looping. Served an honest `poll_log` by `/load_config`, the tool does its one harmless switch (200 OK, nothing changes), then its slcan handshake on the main port gets silence and it errors out on its own timeout. One clean failure per run. **No path to a partial PCM write.**

**Still unconfirmed** (the tool's source is not on this machine): the exact error message the user sees, and whether the tool's error path still posts the restore.

## Other product facts worth keeping

- **NC Flash >= v2.9.0** (2026-06-28) pings port 35001, gates on `NCFRv >= COEXIST_MIN_FW_REV` (=6), and flashes with no mode switch at all: `bus_claim -> pause -> fast op -> bus_release`. This is the only supported path from v1.23.0 on.
- The share of users still on <= v2.8.0 is **unknown**. The original design assumed a non-trivial number; v1.23.0 accepted locking them out deliberately.
- Whether v2.9.0+ **always** arms `bus_claim` before a fast op is unconfirmed. It matters to the overlay design in #145, where the `flash_active || host_bus_claimed` OR makes it moot either way.

## History: what this document used to be, and why it went

It was a three-part goal document written before v1.23.0, titled "Host-session UI lock + Datalogger mode lockdown". Its parts ended up like this:

| Part | What it designed | Outcome |
|---|---|---|
| **A** | Mode lockdown with a one-boot `slcan` grant, so old NC Flash kept working | **Dead.** Superseded by `remove-slcan-mode.md`, which deleted the mode field outright instead. Shipped in v1.23.0 (#141). There is no `protocol` key to grant. |
| **B** | Remove the Console "Bench SLCAN" mode chip | **Done.** #141 removed it. |
| **C** | Full-page host-lock overlay while NC Flash holds the bus | **Alive, unbuilt.** Moved to #145, with its dead `slcan_boot` trigger term dropped. |

The hazard analysis went with Part C to #145. The one that mattered most: `POST /system_reboot` (`main/config_server.c:1209`) reboots with no `can_flash_active()` check, so a stale tab or a stray `curl` can still reset the adapter mid-`TransferData` and leave the PCM unbootable. That is now tracked properly rather than living in an untracked file.

Do **not** revive Part A. The `protocol`, `port` and `port_type` keys still written into `config.json` are a rollback shim with fixed values and no meaning to this firmware; `docs/internals/returning-to-datalogger.md` explains why editing them does nothing.
