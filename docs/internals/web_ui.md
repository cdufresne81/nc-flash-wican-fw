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

**Expression authoring in standard OBD form (issue #61):** Polled-PID `Expression` fields are edited in the standard SAE&nbsp;J1979 / Torque vocabulary — data bytes `A`, `B`, `C`, … counting from the *first data byte* — while the value **stored** in `auto_pid.json` stays the firmware's raw `Bn` indices, which include the ISO-TP framing (`B0`=PCI, `B1`=service echo, `B2`=PID echo, so Mode 01 data begins at `B3`, Mode 22 at `B4`). The map is a per-token swap keyed on the Mode's data offset — `off = 2 + the operand bytes the ECU **echoes**`, from the explicit `MODE_ECHO_BYTES` table (`01`→1, `22`→2, `23`→0, i.e. `B3`/`B4`/`B2`). This was once *derived* from `MODE_IDENT_LEN`, which happened to work while echo length equalled operand length; mode 23 (issue #51) breaks that — it sends a 6-byte operand and echoes **none** of it, so the derivation would have computed `B8` and read past the frame. Hence: `A↔B{off}`, `B↔B{off+1}`, … via `abcToBn`/`bnToAbc` in `main.js`, which are **exact inverses** on the standard subset; the friendly A/B/C form stays in the box and `rowStoredExpr()` translates it back to raw `Bn` only at save/test time (no live preview). To match the market notation exactly, the firmware's compact range `[Bx:By]` is normalized to the arithmetic `((Bx*256)+By)` form (`rangeToArith`, unsigned ranges ≤ 4 bytes) so the friendly view reads `((A*256)+B)/4` rather than `[A:B]/4` — a **one-time migration** (like `Init`→`Mode` above): a bracket-form config's first Store rewrites `[B3:B4]/4` to `((B3*256)+B4)/4` (identical value — the evaluator computes both the same), after which every load → save is byte-identical. Signed `[Sx:Sy]` and wider ranges stay compact. Only a row whose stored expression is fully representable in `A/B/C` (every byte reference inside the physical response frame — `B3`–`B7` for Mode 01, `B4`–`B7` for Mode 22, since `poll_log` evaluates a single 8-byte frame — no signed `Sn`, no framing byte) is shown friendly and flagged `dataset.exprAbc='1'` (`exprAsAbc`); anything else — and every non-canonical PID, where the Mode/offset isn't trustworthy — stays in raw `Bn`, edited and stored verbatim, exactly like the pre-#31 legacy PID path. Changing a canonical row's Mode re-frames the stored bytes automatically (`B3`↔`B4`) because the friendly form is mode-independent. **Custom CAN filter (broadcast) rows are unchanged:** a raw frame has no framing to strip and no `A/B/C` convention, so it references bytes directly as `Bn` and keeps a fixed clarifying label. The stored schema and the firmware evaluator are untouched.

Hidden this way so far: protocol selector, CAN bitrate/mode, BLE, Battery Alert, Low-Voltage Behavior, Motion Threshold (PR #25); per-PID and per-filter `Class` + `Period(ms)` rows (PR #27 follow-up — inert outside Legacy AutoPID; `Period` stays retired — issue #29 shipped as a *new* `SampleEvery` key rather than re-using it, precisely because every shipped PID carries `Period: "200"`); Periodic wake up + Wakeup Every (PR #46 — the wake was only an `esp_restart()` that nothing distinguishes from a cold boot, and this build has no outbound client to report with; issue #24 would revive it).

## Dynamic entry templates

PID entries and Custom CAN Filter entries are not static HTML — they're template literals in `main.js` (`addCollapsibleRow` and the filter equivalent). Hidden fields there must be hidden **in the template**, and inputs must keep their class names (`.period-input`, `.class-input`, …) because the store functions query by class.

## Config round-trip gotchas

- `GET /load_config` streams the raw stored `config.json` (legacy configs may lack new keys); `GET /check_status` is the *built* JSON with defaults applied. Don't confuse them.
- `POST /store_config` replaces the whole file and **always reboots** — to change one key: load → modify → post the full object.
- `POST /store_auto_data` writes `auto_pid.json` **without** rebooting and hot-swaps the live PID table on the poll task (issue #39). The reply envelope says which happened: `"applied":"live"`, or `"applied":"deferred"` when a CSV trip was open (the swap retries once it closes).
- Per-PID `SampleEvery` (issue #29) is **omitted when it is 0 or 1**, so a config that uses no divisors round-trips byte-identically. Only 2..64 is emitted; the UI validator's cap must stay equal to `AUTOPID_MAX_SAMPLE_EVERY` in `components/autopid/autopid.h`.
- Per-PID `Period` is a **retired, hidden field** and is intentionally **not validated** on save. A new polled PID is created with an empty `Period`. The shipping **POLL_LOG** protocol never reads `Period` (poll_log sweeps every PID each pass); the only reader is the legacy **AUTO_PID** scheduler (a mutually-exclusive protocol slated for removal in #28), which tolerates a blank value. An existing PID's stored `Period` still round-trips through the hidden input. (The old `"Period must be a number greater than 100"` check validated this invisible field and made adding any new polled PID impossible.)
- `csv_grid_hz` is a string key holding `"1".."100"` or `"auto"` (see [csv_logger.md](csv_logger.md)).

## Sensor-set file — Logger export / import (issue #63)

The Logger tab's **Export Sensors** / **Import Sensors** pair is a *sensors-only* backup, distinct from the System tab's whole-device Download/Upload Configuration. The System bundle carries `config.json` — **Wi-Fi credentials included** — and restores through `POST /store_config`, which always reboots; the sensor file lands through `POST /store_auto_data`, which hot-swaps the live table with **no reboot** (issue #39).

The file is **hand-editable YAML written in the page's own vocabulary — not a copy of `auto_pid.json`**:

```yaml
wican: "sensors"
version: 1

polled:
  - name: "VCT_ACT"
    mode: "22"
    pid: "16CD"                       # bare identifier, as the PID box shows it
    expression: "((A*256)+B)*0.0625"  # standard OBD form, A = first DATA byte
    unit: "deg"
    sample_every: 4
    enabled: true

  - name: "VOLEFF"                    # mode 23 = memory read (issue #51)
    mode: "23"
    address: "FFFFAC18"               # address + size, NOT pid: the two boxes the page edits
    size: 4
    expression: "FA"                  # float32 starting at data byte A
    unit: "%"
    enabled: true

broadcast:
  - frame_id: 0x240                   # hex, as you'd read it on the bus
    name: "ECT"
    expression: "B1-40"               # raw frame: no echo to skip, bytes from B0
    unit: "C"
    enabled: true

calculated:
  - name: "AFR"
    expression: "EQ_RATIO_ACT*14.71"
    unit: ""
    enabled: true
```

**Every string is quoted, every number and boolean bare.** Consistency matters more than brevity in a file people edit by hand: an unquoted `01` silently becomes the integer `1`, and an unquoted `%` is a YAML directive marker.

**Keys the Datalogger never reads are not written**, and are restored at their shipped defaults on import (`Period` → `SENSORS_DEFAULT_PERIOD`, `Class` → `"none"`, empty `MinValue`/`MaxValue`) so `auto_pid.json` keeps its schema and existing configs still round-trip:

| Omitted | Why it is dead under poll_log |
|---|---|
| `Period` (polled **and** broadcast) | only the legacy AutoPID scheduler gates on it — `process_can_filter_frame()` is called solely from `autopid_task`, and `fastlog_decode_frame()` is explicitly its equivalent *without* the period gate |
| `Class` | read only to build an MQTT/HA payload this fork never publishes (`autopid.c:1293`) |
| `initialisation`, `ecu_protocol` | legacy ELM setup; poll_log hardcodes physical addressing (`POLLLOG_TX_ID` `0x7E0`) |
| `standard_pids`, `std_pids` | the standard-PID path is unused and has no UI |
| `disable_on_sleep_voltage`, `pid_polling_min_voltage` | real settings, but *not sensors* — omitting them means importing someone else's set never changes your voltage thresholds |

- **Export renders `buildAutoTableJson()`, not the DOM directly.** `emitSensorsYaml()` runs the same serializer `Store` posts, so an invalid row throws at export time instead of producing an unrestorable file, and the friendly/raw decision reuses the identical predicates (`parsePidText`, `exprAsAbc`) the row builder used to decide what to display. Unsaved edits export as displayed.
- **A row is decomposed only when it can be.** `sensorPidParts()` requires a canonical wire string **and** the default one-response-frame hint; anything else (exotic service, odd shape, non-default hint) keeps the full wire string, because the decomposed form has nowhere to put the difference. Independently, an expression that isn't A/B/C-representable (signed `Sn`, out-of-frame byte) stays raw `Bn`. On import `sensorExprIsWire()` tells them apart on the digit: `B3` is a wire byte, a bare `B` is a data byte.
- **A polled expression must be entirely one convention.** `sensorExprIsWire()` is all-or-nothing per row (the same rule the UI's `exprAbc` flag uses): if any token names a numbered byte the whole expression is taken as wire form. A hand edit mixing them — `A*256+B3` — stores the `A` untranslated, which the firmware cannot evaluate. Write `((A*256)+B)` or `((B3*256)+B4)`, never both.
- **`parseSensorsYaml()` is a subset parser, deliberately.** It reads exactly the shape the emitter writes — top-level scalars plus three block sequences of flat maps — and **rejects** anything else with a line number. A general YAML implementation would add tens of KB to a `main.js` that ships uncompressed inside the firmware image, to support constructs (anchors, flow collections, multi-line scalars) this format never emits; silently guessing at a hand edit would reshape someone's sensor set. It does handle comments, blank lines, both bare and quoted scalars, `0x` hex, CRLF, and rejects tab indentation by name.
- **Import is review-then-Store, never a direct write.** `applySensorsFile()` validates the envelope, calls `loadAutoTable()` to *replace* all three tables, then enables Store. The device is untouched until the user presses **Store**.
- **`autoTableSavedJson` is preserved across the import, not cleared.** That variable means exactly one thing — *the table the device is running* — and `loadAutoTable()` re-snapshots it from whatever it just loaded. `applySensorsFile()` therefore captures it beforehand and restores it after, then re-runs `pollSweepRefresh()`. Both consumers depend on it: the combined Submit's `skipIfUnchanged` path (a cleared baseline is also correct there, since `null` never compares equal) and, less obviously, `sampleLoadCommitted()`, which falls back to the live DOM when the baseline is `null` — making `predictedSweepMs()` compute `now/base` with `base === now` and render the "predicted after apply" sweep as if the device already ran the imported set.
- **A wrong file is rejected, never a silent no-op.** Text starting with `{` or `[` is named as JSON (the pre-release format and the whole-device backup both look like that) and pointed at the right tool. A missing `wican: "sensors"` marker throws; a `version` newer than the reader is refused rather than half-read; a missing or non-numeric `version` is treated as "not our file" rather than as a newer build.
- **The guard is reciprocal.** `uploadCfg()` (System → Upload Configuration) rejects a sensor file too, matched on the **raw text before `JSON.parse`** — parsing YAML as JSON first would fail with "Failed to parse configuration file" and never name the tool that does read it.
- `loadAutoTable()` resets **`.pid-entries` as well as** `.custom-canfilter-entries` and `.calculated-entries`. The polled reset is inert at page load (the container starts empty) but mandatory for import, or polled rows would append while the other two tables were replaced.
- **Round-trip:** every *live* field survives load → export → import → Store unchanged — names, modes, PIDs, expressions, units, `SampleEvery`, `enabled`, notes, `min`/`max`, and the logger settings the file does not carry (those are read from the page and passed through, so an import never moves a voltage threshold).

  It is **not byte-identical** for the inert metadata the format deliberately drops. On import, `Class` and `Period` come back as `"none"` / `SENSORS_DEFAULT_PERIOD` on **every** row, polled and broadcast. A config whose broadcast parameters carried real values (`class: "temperature"`, `period: "1000"`) loses them the first time it is exported and re-imported. Nothing under poll_log reads either field — `process_can_filter_frame()`, the only `period` consumer, runs solely under the legacy `autopid_task`, and `class` only feeds an MQTT payload this fork never publishes — so the *behaviour* is unchanged, but the stored bytes move. This is a property of **import only**: an ordinary Store still round-trips those fields verbatim through their hidden inputs. Accept it as the cost of a file that shows only what the Datalogger actually uses; the alternative is carrying dead keys in a human-facing format. Both die for real with issue #28.

`downloadTextFile()` is the shared Blob→download helper behind this export, the System-tab `downloadCfg()`, and the Tactrix `logcfg.txt` exporter to come (issue #37).
