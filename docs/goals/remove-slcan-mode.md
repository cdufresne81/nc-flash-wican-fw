# Goal: The protocol field becomes a placebo — the device always runs the Datalogger

**Status:** READY TO IMPLEMENT. Supersedes the earlier draft of this file (coerce-slcan-only) and the one-boot-grant design in `docs/goals/host-lock-ui-and-mode-lockdown.md` Part A.

**Branch:** new branch off `wican-pro`.

**/goal condition (paste-ready):**

/goal Implement docs/goals/remove-slcan-mode.md: the `protocol` config field is validated for presence but its value is ignored — the parser pins it to "poll_log" in RAM, config_server_protocol() returns POLL_LOG unconditionally, the SmartConnect protocol override and the EVL_MODE emission and config_server_protocol_str() are deleted, the SLCAN router arms and the SLCAN arms in the home/drive getters and config_server_protocol_name() and #define SLCAN are deleted, /load_config and /host_caps report the hardcoded "poll_log", the parser remaps a stock port of 35001 to the default port in RAM; the web UI loses the protocol select row, the PID-warning banner with the Restore Datalogger button, strandRestoreDecision()/restoreDataloggerMode() and their test file, postConfig() hardcodes protocol:"poll_log", and the port field refuses 35001. The dedicated port-35001 SLCAN listener and its dispatch, main/slcan.c, main/slcan_port.c, main/ncflash_fastread.c, main/ncflash_fastwrite.c and main/can.c are untouched. Verified when: idf.py build exits 0, python tools/build_web.py --check and python tools/lint_web.py --check exit 0, node --test tools/webtest passes, and every grep and bench curl output listed under Acceptance criteria appears in the transcript.

## The goal

The owner's decision, in his words: *"we should completely ignore that field, discard it"* and *"Keep the field if you must but at this point it's a placebo, the value should be hardcoded and completely ignored."*

So this is not "remove the slcan value". It is: **the device has exactly one mode, the Datalogger (`poll_log`), hardcoded. Nothing on the device reads `protocol` for a decision, ever.** The key stays present in config.json and in every export/import purely so old backups still load and a firmware downgrade still works.

Why hardcoding beats coercing one bad value: `config_server_protocol()` (main/config_server.c:489-511) matches four known strings and falls through to `return OBD_ELM327` at :510. Verified — so today ANY unknown value ("potato", a truncated write, a future firmware's word) silently boots the device into ELM327 mode with the datalogger dead. A slcan-only coercion would leave that hole open. Ignoring the field closes slcan and every other bad value at once, with no forbidden-word list to maintain.

There are TWO things called SLCAN in this codebase. Only ONE dies:

1. **`protocol: slcan` — the device MODE.** Dies, along with every other mode choice. This is the strand from issue #92.
2. **The always-on SLCAN listener on TCP port 35001. Stays, untouched.** Started unconditionally at `main/main.c:1091` (`slcan_port_init`), dispatched at `main/main.c:279` (`dev_channel == DEV_SLCAN_PORT`) BEFORE the mode gate, RX-forwarded at `main/main.c:439`. NC Flash >= v2.9.0 flashes the car's PCM through this port with no mode switch. Removing any of it makes ECU flashing impossible.

**Accepted consequence, decided by the owner — do not re-open it:** NC Flash <= v2.8.0 loses all ECU access, permanently. Its only route was switching the device into the slcan mode; with the mode gone there is nothing to switch into. No grant, no shim.

## Design

### The one mechanism: pin at the parse site

Every config value passes through one parser, `config_server_parse_cfg_into` (main/config_server.c:2747-2772 for this key). Boot (config_server.c:3493), the /store_config shadow validation (config_server.c:1020) and the live-apply diff all reuse it — the in-code comment at :2757-2766 says exactly that.

**Change 1 — the pin.** Keep the presence and length checks at :2747-2755 exactly as they are (the missing-key `goto config_error` is what keeps old backups loadable and downgrade-symmetric — do not loosen or tighten them). Then replace the strlcpy-plus-auto_pid-coercion at :2756-2771 with an unconditional `strlcpy(dst->protocol, "poll_log", sizeof(dst->protocol))` and a short comment: the field is a placebo, kept only so backups round-trip and downgrades work; the value on disk is never read for a decision. The auto_pid block is subsumed and disappears.

This one line is the hardcode. `device_config.protocol` becomes the constant `"poll_log"`, so every reporter that reads it is truthful with no further work: /check_status (config_server.c:1600), /host_caps (config_server.c:1745). It also makes the live-apply diff blind to protocol changes: a posted `"slcan"` pins to `"poll_log"` in the shadow, equal to live, so a protocol-only store can never even cause a reboot.

**Change 2 — `config_server_protocol()` collapses** (config_server.c:489-511) to `return POLL_LOG;` with a comment. The string compares and the dangerous `OBD_ELM327` fallback are deleted. Verified callers and what they do afterwards:
- `main/main.c:877` — the boot dispatch; always POLL_LOG (see change 4).
- `components/autopid/autopid_http_test_pid.c:252` — takes the in-band poll_log test path (:253-254), today's normal-device behavior.
- `components/autopid/autopid_http_test_can_filter.c:572` — same family; implementer confirms it behaves for POLL_LOG (it did before on every real device).
- `main/config_server.c:2278` (scan_available_pids) — `!= AUTO_PID` stays true; the polite "set AutoPID" message, unchanged from today.

**Change 3 — protocol ids.** Delete `#define SLCAN 0` (config_server.h:67) — the compiler then proves no reference survived. Keep `OBD_ELM327`, `AUTO_PID`, `FAST_LOG`, `POLL_LOG` defined: dead-but-compiled router arms still name them, and the ids must never shift (the #5 trim precedent for ids 1 and 2 — extend that comment to cover 0). Verified there is no zero-value trap: `protocol` in main.c initializes to `OBD_ELM327` (main.c:121), device_config stores the protocol as a string, and the old fallback was `OBD_ELM327`, not 0 — nothing anywhere defaults a protocol id to 0.

**Change 4 — main.c boots one mode, and SmartConnect can no longer change it.** Keep `protocol = config_server_protocol();` (main.c:877) — now constant POLL_LOG. Delete the SmartConnect override block (main.c:881-892), which is today the only code that rewrites the running protocol (to AUTO_PID or OBD_ELM327); SmartConnect's WiFi behavior is untouched. The home/drive getters (config_server.c:381-396, 413-425) lose their SLCAN arms (:383-385, :415-417 — forced by change 3); their remaining consumers, `main/smartconnect.c:304-308` and `:418-422`, only compare against OBD_ELM327/AUTO_PID and keep behaving identically (a returned SLCAN already landed on the OBD_ELM327 path everywhere).

**Change 5 — SLCAN router arms out.** Delete `main/main.c:295-322` (the mode's slcan dispatch; the `else if(protocol_feeds_elm327(...))` at :323 becomes plain `if`) and `main/main.c:455-458` (the slcan TX arm; mind the `#if HARDWARE_VER != WICAN_PRO` `else if` at :459-465, which becomes a plain `if` inside its guard). The `DEV_SLCAN_PORT` dispatch at :279-294 and the coexist RX-forward at :432-447 directly beside them stay byte-for-byte. The now-dead elm327/AUTO_PID/FAST_LOG arms (main.c:323-370, :450-486, :936-1013) are LEFT IN PLACE, compiled but unreachable, for a follow-up cleanup issue — especially do not "simplify" the gate at main.c:393: it parks this task for the flash path (`can_flash_active()`), and rewriting it risks the one thing this change must not touch. `elm327_set_read_task_enabled(protocol_feeds_elm327(protocol))` at :934 now computes false — which is already its value on every real device, and the safe direction (the UART-lock wake bug lived on the other side).

**Change 6 — reporting is the hardcoded truth everywhere.**
- `/check_status` (config_server.c:1600) and `/host_caps` (config_server.c:1745): no code change — they serve `device_config.protocol`, pinned to "poll_log" by change 1. Update the /host_caps contract comment (config_server.c:1723-1727): "protocol is the STORED mode" is retired; the field now always says `poll_log` because there is no other mode a reboot could produce. The host's stranded-device sweep reads that as "healthy", which is now simply true.
- `GET /load_config` (`load_config_handler`, config_server.c:1164-1204): it already cJSON-parses the stored file to redact secrets; in the same pass, set the `protocol` item to the literal `"poll_log"` (`cJSON_SetValuestring`; add the key if somehow absent). This matters three ways: old NC Flash's `current_protocol()` reads this endpoint, and an honest "poll_log" is what makes it fail cleanly (see hazards); downloaded backups can never carry a word that means nothing; and the file on the SD card may keep saying "slcan" (the store path writes payloads verbatim, config_server.c:1031-1049, and the RAM-only rule forbids rewriting it) without that lie ever reaching anyone.
- **The boot MODE line is retired.** Delete the `event_log_emit(EVL_MODE, "stored=%s running=%s ...")` at main.c:905-908 and its comment block (:894-904). Its whole purpose was dating a mode flip; a flip is now impossible and a constant line every boot is noise — the BOOT line already dates the boot. Keep the `EVL_MODE` enum member (event_log.h:65) and its "MODE" name (event_log.c:104) reserved with a comment: the codes sit in the middle of an enum that is persisted in on-SD logs, and deleting the member would shift every later code and misread old logs.
- `config_server_protocol_str()` (config_server.c:471-474): its only caller was that MODE line — delete the function and its declaration (config_server.h:210-214).
- `config_server_protocol_name()` (config_server.c:476-487): delete `case SLCAN: return "slcan";` (:480). The function keeps its other cases for the dead arms' sake.

**Change 7 — the port-35001 self-lockout (new hazard, fixed here).** `main/main.c:1086-1089`: when the stock TCP/UDP port is configured to 35001, the coexistence listener is deliberately NOT started, and the comment's consolation — the host "falls back to the legacy reboot path" — stops existing with this change. A user typing 35001 into the CAN port field would silently make ECU flashing impossible. Two fences, both:
- **Firmware:** in the parser, right after the `port` strlcpy (config_server.c:2723-2732): if the value parses to 35001, pin the RAM copy to the default port ("3333") with an `ESP_LOGW` ("port 35001 is reserved for NC Flash; using 3333"). RAM-only, same rules as the protocol pin. The listener then always starts; the main.c:1086 guard stays as belt-and-braces (now unreachable). /check_status serves the remapped port (config_server.c:1596), so the mismatch against the form value is visible, not silent.
- **Web UI:** postConfig() refuses to submit when the port field says 35001, with a plain message ("Port 35001 is reserved for NC Flash (ECU flashing). Choose another port."). This is the fence users actually meet. Do NOT override the port in /load_config — unlike protocol, port is a real setting the user must see and fix; only the placebo gets the reporting override.

The owner's own device is on 35000 — one digit away — which is why this is in scope and not a footnote.

### Web UI (edit `main/web/homepage_full.html` — the authoritative source — and `main/web/src/main.js`; regenerate `main/web/src/homepage.html` with `python tools/build_web.py`, never by hand; `python tools/lint_web.py` must pass)

**Change 8 — the protocol select row is deleted** (homepage_full.html:1648-1661, comment included). postConfig() hardcodes `obj["protocol"] = "poll_log";` (replacing main.js:3382) with a comment: placebo key, kept so backups load on any firmware. checkStatus() drops the select mirror (main.js:3252). This also answers the open question (a): `fast_log` and `elm327` do not survive in the dropdown because the dropdown itself is gone — with the field ignored, a selector would be a lie.

**Change 9 — the strand recovery machinery is torn back out.** This deletes what PR #121 shipped two days ago — said plainly, because it is the owner's shipped work being reversed: the "PID Polling Inactive" banner with the "Restore Datalogger mode" button (homepage_full.html:1776-1783), its display toggle (main.js:3257-3261), `strandRestoreDecision()` and `restoreDataloggerMode()` (main.js:3484-3605), and `tools/webtest/strand_restore.test.mjs`. Rationale: the banner keys off /check_status reporting a non-recording protocol, which after change 1 can never happen — every line of it is unreachable, and dead recovery UI misleads the next maintainer into thinking a strand is still possible. (Fallback if the owner prefers zero churn: keep it all, it can never display. The acceptance criteria below assume deletion; the owner picks before running /goal.) lint_web's onclick pairing check forces the HTML button and the JS function out together anyway.

**Change 10 — console chip.** `MODE_NAMES` (main.js:4723) shrinks to `{poll_log:'Datalogger'}`; the raw-string fallback at :4726 stays. The chip now constantly — and truthfully — says "Datalogger".

**Confirmed while sweeping:** there is no "expert settings" mode anywhere in this UI (grep over main/web finds nothing); the hidden select was the only thing that phrase could mean, and change 8 removes it.

## Behavior after the change

| Situation | Result |
|---|---|
| Device stranded in slcan gets this firmware (OTA) | Next boot runs poll_log. Un-stranded forever, automatically. |
| Any backup uploaded — protocol slcan, elm327, fast_log, "potato" | Accepted (200 if otherwise valid). Device runs poll_log. Exported backups always say poll_log. |
| Old NC Flash (<= v2.8.0) | /load_config says "poll_log" -> tool decides to switch (once — see hazards) -> /store_config answers 200, no reboot (the pin makes protocol invisible to the live-apply diff) -> tool talks slcan on the main port -> poll_log routing ignores the bytes -> the tool times out and errors. It cannot reach the flash stage, so it cannot half-write the PCM. |
| NC Flash >= v2.9.0 | Unchanged. Port 35001, claim/park, fast read/write exactly as today. |
| SmartConnect | Can no longer touch the mode (override deleted). WiFi behavior unchanged. |
| User sets the CAN port to 35001 | UI refuses. Via curl/import: RAM remaps to 3333, listener starts, /check_status shows 3333. |
| Firmware downgrade | config.json still carries a valid `protocol` key (whatever was last stored), so old parsers load it. See hazard 3 for the value it may carry. |

## Hazards, most severe first

1. **RED — touching the port-35001 path by mistake.** Consequence: NC Flash >= v2.9.0 can no longer flash the car at all; and if the `DEV_SLCAN_PORT` dispatch (main.c:279-294), the coexist RX-forward (main.c:432-447), the gate at main.c:393, or the FLASH_ACTIVE serialization around them is disturbed, a live PCM write can be corrupted mid-TransferData — a dead engine computer. This design touches none of it; the Constraints list and the greps in the acceptance criteria prove it in the transcript. If an edit seems to need a change in those files or lines, the edit is wrong.
2. **RED — old NC Flash (<= v2.8.0) is permanently locked out.** Decided and accepted. How it actually fails, and how sure we are: the previous implementer verified against the tool's own source (`nc-flash src/ecu/wican_config.py`, recorded in docs/goals/host-lock-ui-and-mode-lockdown.md:29) that `slcan_session()` calls `set_protocol(SLCAN)` exactly once on entry and `restore()` once on exit — there is no retry loop around the switch. Served an honest "poll_log" by /load_config, it performs its one switch (harmless 200), then its slcan handshake on the main port gets silence and it errors out on its own timeout. One clean failure per run; no infinite reboot cycle (the pin means the store does not even reboot the device); no path to a partial PCM write. Unconfirmed (tool source is not on this machine): the exact error message the user sees, and whether its error path still posts the restore. Put a blunt line in the PR body: only NC Flash v2.9.0+ can flash through this firmware.
3. **ORANGE — firmware downgrade can resurrect a mode.** The store path writes payloads verbatim and RAM-only means the file is never corrected, so config.json can carry `"slcan"` (or `"elm327"`) indefinitely — e.g. after an old-tool attempt. Downgrade to a pre-change firmware and that file boots the old firmware straight into that mode: the slcan strand again, datalogger dead. Mitigation is inherent: any Submit from the new UI persists `"poll_log"` (change 8 hardcodes it), and the owner controls what firmware goes on his own device. Not fully preventable without writing config.json from firmware, which is forbidden (F2 factory-reset hazard — no rename-over on FATFS).
4. **ORANGE — port 35001 self-lockout** (found by the team lead, fixed by change 7). Without the fix: one typo in the port field and the coexistence listener silently never starts — ECU flashing gone, the only clue an ESP_LOGW on a serial console that OBD-PRO does not expose. It is recoverable (change the port back in the UI), but nothing would ever tell the user to. With change 7 the UI refuses the value and the firmware remaps it, so the listener always runs. Residual: the remap itself is only visible as a /check_status-vs-form mismatch, not as an event-log line — the parser also runs on the httpd task for every shadow validation, and emitting events from there would double-log. Accepted.
5. **YELLOW — /store_config still answers 200 to any protocol word.** Deliberate: the same parser validates imports, and rejecting a value would make old backups unrestorable — a lockout of the owner from his own backup. The value is accepted and discarded; the mode is unreachable. Do not "improve" this into a 400.
6. **YELLOW — elm327 and fast_log modes die as a side effect.** Ignoring the field removes them too, not just slcan. Verified consumers: nothing in the UI could select them (hidden select), the BLE path that used elm327 has no UI (memory: WiFi-only platform), and `elm327_set_read_task_enabled` going constant-false is the direction that FIXED the 20 s wake bug. This is inside the owner's "hardcoded and completely ignored" decision, stated here so nobody reads it as an accident.
7. **YELLOW — the boot log goes quieter.** With EVL_MODE retired there is no per-boot mode line and no trace of what the (now meaningless) file says. If a stale "slcan" survives on disk, nothing reports it anywhere — by design, since it decides nothing. The only remaining traces are serial-only ESP_LOGWs. Accepted.

## Acceptance criteria (each verifiable from the session transcript)

Build and static:

1. `idf.py build` exits 0 (output shown).
2. `python tools/build_web.py --check` and `python tools/lint_web.py --check` both exit 0.
3. `node --test tools/webtest` passes; `tools/webtest/strand_restore.test.mjs` no longer exists (`git status` / `ls tools/webtest` shown).
4. Grep shows the pin: the hunk around config_server.c:2756 shows the presence/length checks intact, then the unconditional `strlcpy(dst->protocol, "poll_log", ...)`; and `config_server_protocol()` is shown collapsed to `return POLL_LOG;` with no `OBD_ELM327` fallback.
5. `grep -rn "SLCAN" main/config_server.h main/config_server.c main/main.c` shows: no `#define SLCAN`, no `protocol == SLCAN`, no `case SLCAN`, no `return SLCAN` — while `DEV_SLCAN_PORT` (dispatch ~main.c:279) and `WICAN_DEDICATED_SLCAN_PORT` / `slcan_port_init` (~main.c:1091) still appear.
6. `grep -n "SMARTCONNECT_MODE" main/main.c` shows the override block no longer assigns `protocol`; `grep -n "EVL_MODE" main/main.c` returns nothing, and `grep -n "EVL_MODE" components/event_log/include/event_log.h` shows the enum member still present with a reserved comment.
7. Grep shows the /load_config protocol override to the literal `"poll_log"` (hunk near config_server.c:1180-1192) and the parser's 35001-port remap (hunk near config_server.c:2723-2732).
8. `git diff --stat` shows NO changes to: `main/slcan.c`, `main/slcan_port.c`, `main/slcan_port.h`, `main/slcan.h`, `main/ncflash_fastread.c`, `main/ncflash_fastwrite.c`, `main/can.c`, `main/can.h`.
9. `grep -n "slcan\|protocol" main/web/homepage_full.html` shows no protocol `<select>` and no `restore_datalogger_btn`; `grep -n "strandRestoreDecision\|restoreDataloggerMode\|Bench SLCAN" main/web/src/main.js` returns nothing; grep shows postConfig() containing the hardcoded `obj["protocol"] = "poll_log"` and the 35001 refusal.

Bench, over HTTP to the test device (curl output shown; the bench-test-before-merge rule applies):

10. Baseline: `GET /check_status` shows `"protocol":"poll_log"`.
11. Download the config (`GET /load_config`), set `"protocol":"slcan"`, `POST /store_config` -> 200. With no reboot: `GET /check_status`, `GET /host_caps`, `GET /load_config` ALL show `"protocol":"poll_log"`.
12. `POST /system_reboot`; after the boot: `GET /check_status` shows `"protocol":"poll_log"` — proof the file's value (still "slcan" from step 11) decided nothing.
13. Repeat step 11 with `"protocol":"elm327"` — same result: every endpoint says `"poll_log"`.
14. Port 35001 is alive: open TCP to the device on 35001, send `V\r`, a reply arrives (shown).
15. Port remap: POST a config with `"port":"35001"` -> 200; after reboot `GET /check_status` shows `"port":"3333"` and step 14 still passes. Then restore the real port.
16. Round trip: fresh `GET /load_config` re-POSTed unmodified -> 200; `GET /check_status` shows the same `sta_ssid` and `"protocol":"poll_log"` — nothing lost, and the downloaded file carries `"poll_log"`.
17. Restore the bench device's pre-test config.

## Constraints — what must NOT change

- **The port-35001 coexistence path, byte-for-byte:** the `slcan_port_init` call and its guard (main.c:1086-1094), the `DEV_SLCAN_PORT` dispatch (main.c:279-294), the coexist RX-forward and its gate (main.c:391-447), `main/slcan.c`, `main/slcan_port.c`, `main/ncflash_fastread.c`, `main/ncflash_fastwrite.c`.
- **`main/can.c` / `can.h`:** FLASH_ACTIVE_BIT semantics, claim/park leases, the reaper — untouched. Nothing weakens `can_flash_active()` or the sleep session veto (sleep_mode.c:1094, :1895-1931).
- **No firmware write to config.json** outside the existing /store_config path and the existing parse-failure factory restore (config_server.c:3497-3507). Both pins (protocol, port) are RAM-only.
- **The `protocol` key stays required in the parser** (`goto config_error` when missing, config_server.c:2747-2751) **and present in /load_config output and every export** — a backup without it could never be restored on any firmware.
- **/store_config keeps persisting the client payload byte-verbatim** and keeps answering 200 to a parseable config, whatever its protocol word.
- **Endpoint shapes are unchanged:** every existing key stays in /load_config, /check_status, /host_caps; only the `protocol` VALUE is now constant, and only the RAM port value is remapped.
- The dead elm327/AUTO_PID/FAST_LOG router arms and dispatch branches in main.c stay compiled (follow-up cleanup issue); `protocol_feeds_elm327()` and the `OBD_ELM327`/`AUTO_PID`/`FAST_LOG`/`POLL_LOG` defines stay; the `EVL_MODE` enum member stays.
- `main/web/src/homepage.html` only via `tools/build_web.py`; no new FreeRTOS tasks; OTA fence (`s_ota_active`) and the #86/#126/#134 interlocks untouched.

## Manual / bench checklist (human eyes or real tools — OUTSIDE the /goal condition)

- [ ] A real NC Flash >= v2.9.0 session end-to-end against the bench PCM: version ping, fast read, claim/release — all over 35001, no regression.
- [ ] If a v2.8.0-or-older NC Flash build is available: run it once; confirm it fails with a clean error (no hang, no endless retry) and the device stays a working datalogger afterwards.
- [ ] Browser pass: Settings shows no protocol control anywhere; the port field refuses 35001 with the message; the Console chip says "Datalogger"; no banner or Restore button exists on the Logger tab.
- [ ] A CSV trip records normally after the change.
- [ ] Restore the bench device's real sleep config if it was touched (bench is on TEST values 14.5 V / 1 min).

## Unconfirmed (flagged, not dropped)

- The exact user-facing error an NC Flash <= v2.8.0 shows when its slcan handshake times out, and whether its error path still posts the protocol restore (tool source not on this machine; the once-per-session switch structure IS verified, per docs/goals/host-lock-ui-and-mode-lockdown.md:29).
- Whether any third-party script reads /load_config expecting the stored protocol word specifically; every consumer found in this repo wants — or is indifferent to — the constant.
- `autopid_http_test_can_filter.c:572`'s exact branch for POLL_LOG (assumed same in-band pattern as test_pid; implementer confirms).
- Whether the owner wants change 9's fallback (keep the dead banner) instead of deletion; the criteria assume deletion.
