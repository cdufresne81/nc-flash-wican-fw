# Web UI — build pipeline and the streamline pattern

## Source of truth and build

| File | Role |
|---|---|
| `main/web/homepage_full.html` | **Hand-edited source of truth** for the page structure |
| `main/web/src/main.js` | Hand-edited application JS (served as its own file) |
| `main/web/src/homepage.html` | **Generated** minified page — never edit by hand |
| `tools/build_web.py` | Regenerates `homepage.html` from `homepage_full.html` |
| `tools/lint_web.py` | 3 mandatory gates (below) |

Workflow for any UI change: edit `homepage_full.html` and/or `main.js` → `python tools\build_web.py` → `python tools\lint_web.py` → commit all three files. The lint gates are: (1) every `getElementById` literal resolves to an existing `id`, (2) every inline handler call resolves to a defined function, (3) `build_web --check` confirms the generated file is current. CI-independent — run them locally, always.

The UI is embedded in the firmware image, so a UI-only change still requires a build + OTA flash to appear on a device.

## The streamline pattern: hide, don't delete

Since PR #25 this product presents a trimmed UI (Datalogger-only). The discipline for removing something from view:

1. **Keep the element in the DOM**, hidden (`style="display:none"` on the row, or a hidden wrapper).
2. **Keep the save path sending its value** — either the hidden input still feeds `postConfig()`/`storeAutoTableData()`, or the key is captured at load into the `loadedPassthrough` list in `main.js` and re-sent verbatim.
3. **Write an in-code comment at the hide site** saying *why* it's hidden and what still consumes the value. These comments are the ground truth; issue #28 is the index.

This guarantees a browser save/revert cycle leaves `/load_config` and `/load_auto_pid` **byte-identical** — the invariant used to validate every streamline change. Verify with a load → save → diff of the two JSON files (mask credentials when diffing).

**Known exceptions to the byte-identical invariant:** `batt_alert` and `periodic_wakeup` are *retired*, not merely hidden — `config_server_parse_cfg_into()` force-pins both to `"disable"`, so a device whose stored config had either set to `"enable"` shows that one key flipped after a save/revert cycle. Intentional; not a regression. Per-PID `Init` → `Mode` (issue #31) is a **one-time migration**: the first Store after the update drops each pid's `"Init"` key (free-text ATSH strings the Datalogger protocol never executed) and adds `"Mode"`, always emitted. The **stored** `PID` stays the full wire string (`010C1`, `2217461`) — what poll_log frames verbatim and what `pid_prefix_mode()` in `autopid_config.c` derives the mode from — but the **UI shows it decomposed**: the Mode dropdown carries the service byte, the PID box shows only the identifier (`0C`, `1746`), and the trailing ELM frames-hint nibble is hidden (preserved per row, re-attached on save; dies for real with #28). `parsePidText()`/`composePidText()` in `main.js` are exact inverses on canonical rows, so the cycle stays byte-stable after the first save. A stored PID that doesn't parse canonically (exotic service, odd shape) keeps the pre-#31 UX — full string in the box, Mode display-only, saved verbatim with `Mode` derived from the prefix, console-warned, never blocked. A dropped non-`ATSH7E0;` Init is `console.warn`ed at load. `csv_grid_mode` (issue #53) is likewise *deleted*, not hidden: the Event grid mode is gone, the firmware neither parses nor emits the key, the UI doesn't send it — so a pre-#53 `config.json` loses the key on its first Submit. Intentional; not a regression.

**Expression authoring in standard OBD form (issue #61):** Polled-PID `Expression` fields are edited in the standard SAE&nbsp;J1979 / Torque vocabulary — data bytes `A`, `B`, `C`, … counting from the *first data byte* — while the value **stored** in `auto_pid.json` stays the firmware's raw `Bn` indices, which include the ISO-TP framing (`B0`=PCI, `B1`=service echo, `B2`=PID echo, so Mode 01 data begins at `B3`, Mode 22 at `B4`). The map is a per-token swap keyed on the Mode's data offset (`off = 2 + ident_bytes`, derived from `MODE_IDENT_LEN`): `A↔B{off}`, `B↔B{off+1}`, … via `abcToBn`/`bnToAbc` in `main.js`, which are **exact inverses** on the standard subset, and a live "stored as `((B3*256)+B4)/4`" preview under the box shows the exact bytes a Store will write. To match the market notation exactly, the firmware's compact range `[Bx:By]` is normalized to the arithmetic `((Bx*256)+By)` form (`rangeToArith`, unsigned ranges ≤ 4 bytes) so the friendly view reads `((A*256)+B)/4` rather than `[A:B]/4` — a **one-time migration** (like `Init`→`Mode` above): a bracket-form config's first Store rewrites `[B3:B4]/4` to `((B3*256)+B4)/4` (identical value — the evaluator computes both the same), after which every load → save is byte-identical. Signed `[Sx:Sy]` and wider ranges stay compact. Only a row whose stored expression is fully representable in `A/B/C` (every byte reference inside the physical response frame — `B3`–`B7` for Mode 01, `B4`–`B7` for Mode 22, since `poll_log` evaluates a single 8-byte frame — no signed `Sn`, no framing byte) is shown friendly and flagged `dataset.exprAbc='1'` (`exprIsAbcCanonical`); anything else — and every non-canonical PID, where the Mode/offset isn't trustworthy — stays in raw `Bn`, edited and stored verbatim, exactly like the pre-#31 legacy PID path. Changing a canonical row's Mode re-frames the stored bytes automatically (`B3`↔`B4`) because the friendly form is mode-independent. **Custom CAN filter (broadcast) rows are unchanged:** a raw frame has no framing to strip and no `A/B/C` convention, so it references bytes directly as `Bn` and keeps a fixed clarifying label. The stored schema and the firmware evaluator are untouched.

Hidden this way so far: protocol selector, CAN bitrate/mode, BLE, Battery Alert, Low-Voltage Behavior, Motion Threshold (PR #25); per-PID and per-filter `Class` + `Period(ms)` rows (PR #27 follow-up — inert outside Legacy AutoPID; `Period` stays retired — issue #29 shipped as a *new* `SampleEvery` key rather than re-using it, precisely because every shipped PID carries `Period: "200"`); Periodic wake up + Wakeup Every (PR #46 — the wake was only an `esp_restart()` that nothing distinguishes from a cold boot, and this build has no outbound client to report with; issue #24 would revive it).

## Dynamic entry templates

PID entries and Custom CAN Filter entries are not static HTML — they're template literals in `main.js` (`addCollapsibleRow` and the filter equivalent). Hidden fields there must be hidden **in the template**, and inputs must keep their class names (`.period-input`, `.class-input`, …) because the store functions query by class.

## Config round-trip gotchas

- `GET /load_config` streams the raw stored `config.json` (legacy configs may lack new keys); `GET /check_status` is the *built* JSON with defaults applied. Don't confuse them.
- `POST /store_config` replaces the whole file and **always reboots** — to change one key: load → modify → post the full object.
- `POST /store_auto_data` writes `auto_pid.json` **without** rebooting and hot-swaps the live PID table on the poll task (issue #39). The reply envelope says which happened: `"applied":"live"`, or `"applied":"deferred"` when a CSV trip was open (the swap retries once it closes).
- Per-PID `SampleEvery` (issue #29) is **omitted when it is 0 or 1**, so a config that uses no divisors round-trips byte-identically. Only 2..64 is emitted; the UI validator's cap must stay equal to `AUTOPID_MAX_SAMPLE_EVERY` in `components/autopid/autopid.h`.
- `csv_grid_hz` is a string key holding `"1".."100"` or `"auto"` (see [csv_logger.md](csv_logger.md)).
