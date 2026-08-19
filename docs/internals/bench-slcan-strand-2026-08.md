# Investigation plan: the Bench SLCAN strand (August 2026)

Dated snapshot — a record of where this investigation stood and what to do next. See
[returning-to-datalogger.md](returning-to-datalogger.md) for the evergreen explanation of the
mechanisms. Tracks issue #92, with #109, #70 and #69 alongside.

## The question

Issue #92: the device was found in Bench SLCAN mode, had stopped datalogging, and only a hand
edit of the stored configuration brought it back. **What put it there?**

## Hypothesis

NC Flash's wireless path probes the device's always-on coexistence port (35001) with a **1500 ms**
timeout. On **any** probe failure — including a brief network hiccup — it falls back to the legacy
path, which writes `protocol: slcan` into stored configuration and restores the original only on a
clean disconnect or app exit. A session killed in between leaves the device stranded, and three
recovery defects make that permanent (breadcrumb deleted even when the restore throws; recovery
only attempted on the next connect; breadcrumb in OS temp, keyed by IP).

## Evidence as of 2026-08-18

**For:**

- The fallback is real and has fired on this setup: `~/.nc-flash/nc-flash.log`, **2026-07-12
  12:57:20** — `WiCAN dedicated port answered rev=None (< NCFRv6); legacy reboot path`.
  `rev=None` means the probe got nothing parseable, not old firmware — the same device reported
  NCFRv6 on 57 other sessions.
- The WiCAN events.log attached to #92 ends on a `HOST_CLAIM` + `DATALOG_PARK` with no matching
  release, where every earlier pair released within seconds: a host session that died.

**Against / missing:**

- **No NC Flash log covers 2026-08-11 at all.** All 31 session files plus the main log were
  searched; only one file touches that window and holds a single unrelated line. Yet the WiCAN
  log shows a real flash that day (`FLASH_OK ... blocks=390`, 15:12). So the instance that did
  the 08-11 flash was not logging to `~/.nc-flash` — packaged build, another machine, or a
  different log config.
- Boot event lines carry no mode field, so the flip cannot be dated from the device either.

**Verdict: mechanism proven to exist and to have fired once; the 08-11 incident is unattributed.**
Alternative causes not excluded: another machine's NC Flash, a restored config backup, a hand edit.

## Plan

### Stage 0 — close the observability gap (do first)

Without this, neither a recurrence nor a fix can be confirmed in the field.

- Firmware: put the running mode on the BOOT event line.
- NC Flash: log the path decision and every protocol write/restore at INFO; make packaged builds
  log to `~/.nc-flash`.

### Stage 1 — host-side unit tests (no hardware, seconds) — START HERE

In nc-rom-editor, run with `venv-windows\Scripts\python.exe -m pytest`. Each asserts the
**desired** behaviour, so it is RED today and GREEN after the fix:

1. Probe raises or times out → assert it does **not** silently switch modes.
2. `restore()` raises → assert the breadcrumb file **still exists** (today it is deleted in a
   `finally`).
3. Breadcrumb present at start-up, no connect → assert recovery runs (today no sweep exists).

If all three pass unchanged, the theory is wrong and this plan stops here.

### Stage 2 — deterministic bench reproduction

Do not wait for a hiccup: put a TCP proxy in front of the device that delays port 35001 past the
1500 ms probe timeout while leaving port 80 normal. Drive `ECUSession` directly from a test so it
is scriptable rather than a GUI dance.

Pass criteria, in order: baseline `/check_status` reads `poll_log` → connect through the proxy →
NC Flash logs `legacy reboot path` **and** `/check_status` now reads `slcan` → hard-kill the
process → `/check_status` still reads `slcan`. Then relaunch to test whether recovery fires, and
again with the breadcrumb deleted to prove it does not.

### Stage 3 — firmware-side defects, independently

- **#69**: `curl --data-binary @fw.bin http://<ip>/upload/ota.bin` → expect 500 → `/poll_status`
  shows inactive. Recovery is a reboot.
- **#70**: pause the logger, then poll `bus_idle_ms` for 60 s key-on and key-off; assert it never
  reaches 300 key-on, which is why the reaper cannot fire while driving.
- **Hidden selector**: with the device in `slcan`, change an unrelated setting in the web UI and
  Save; assert `/check_status` still reads `slcan`.

### Stage 4 — keep it

Stage 1 tests in nc-rom-editor CI; Stage 2 marked as a hardware test, run on demand.

## Hazards

- Stage 2 **deliberately strands the bench device in Bench SLCAN**. Recovery is a config edit plus
  a reboot; the test teardown must restore it.
- Stage 3's #69 proof **deliberately kills CAN until a reboot**. Bench only, never on a car in use.
- Never run any of this while an ECU flash is possible.
- The bench is on test sleep values (14.5 V / 1 min), so sleep can interrupt a run — disable sleep
  first.

## What this will still not prove

That 08-11 was this. Only Stage 0 makes the next occurrence attributable.

## Process note

Per the standing rule, the fix design goes to Fable before any of it is implemented.

## Where the code is

So a fresh session does not have to re-derive any of this. Symbols, not line numbers — lines
drift.

**NC Flash — `C:\Users\dufre\Projets\nc-rom-editor`** (this is where Stage 1 happens):

| What | Where |
|---|---|
| The probe, and the fallback comment that says ANY failure takes the legacy path | `src/ecu/session.py` → `_try_open_coexist_port()` |
| The connect path that chooses coexist vs legacy | `src/ecu/session.py` → `connect_ecu()`, the `_wican_auto_config and not self._slcan_switched` branch |
| Writes the breadcrumb then switches the device to slcan | `src/ecu/session.py` → `_enter_slcan_durable()` |
| Restore on disconnect — **clears the breadcrumb in a `finally`, even when restore throws** | `src/ecu/session.py` → `_restore_wican_protocol()` |
| Same defect in the context-manager form | `src/ecu/wican_config.py` → `slcan_session()` |
| Breadcrumb read / write / delete, and its OS-temp, IP-keyed path | `src/ecu/wican_config.py` → `read_recovery()`, `_write_recovery()`, `clear_recovery()`, `_host_keyed_temp_path()` |
| The one-token config edit that persists the mode | `src/ecu/wican_config.py` → `set_protocol()`, `set_top_level_protocol()` |
| The 1500 ms probe timeout, the port, the min firmware rev | `src/ecu/constants.py` → `COEXIST_PROBE_TIMEOUT_MS`, `WICAN_DEDICATED_SLCAN_PORT`, `COEXIST_MIN_FW_REV` |
| Test runner (PATH python lacks PySide6) | `venv-windows\Scripts\python.exe -m pytest` |

**Firmware — this repo:**

| What | Where |
|---|---|
| Mode resolved once at boot; the SmartConnect override | `main/main.c`, the `config_server_protocol()` call and the `SMARTCONNECT_MODE` block after it |
| Mode is not live-appliable, so a change always reboots | `main/config_server.c` → `LIVE_APPLY_WHITELIST` |
| Status reports the **stored** mode, not the running one | `main/config_server.c` → `config_server_get_status_json()` |
| Failed OTA leaves the bus off (#69) | `main/config_server.c` → `upload_post_handler()`, every `return ESP_FAIL` after its `can_disable()` |
| The reaper and its four preconditions | `main/datalog_lease_task.c` |
| Lease/flag primitives, and the idle clock behind #70 | `main/can.c`, `main/can.h` → the `COEXIST_*` constants |
| Hidden mode selector that round-trips its value | `main/web/src/main.js` → the `checkStatus()` populate and the `postConfig()` re-send (never hand-edit the built page) |

**Evidence files:**

- NC Flash logs: `~/.nc-flash/nc-flash.log` and `~/.nc-flash/logs/*.log`. The decisive greps are
  `legacy reboot path`, `coexistence firmware NCFRv`, and `protocol restore failed`.
- The device log for #92 is attached to the issue itself; fetch it with `gh` rather than trusting
  any local copy.
