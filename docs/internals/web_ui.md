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

**Known exceptions to the byte-identical invariant:** `batt_alert` and `periodic_wakeup` are *retired*, not merely hidden — `config_server_parse_cfg_into()` force-pins both to `"disable"`, so a device whose stored config had either set to `"enable"` shows that one key flipped after a save/revert cycle. Intentional; not a regression.

Hidden this way so far: protocol selector, CAN bitrate/mode, BLE, Battery Alert, Low-Voltage Behavior, Motion Threshold (PR #25); per-PID and per-filter `Class` + `Period(ms)` rows (PR #27 follow-up — inert outside Legacy AutoPID; `Period` is reserved for the sample-groups feature, issue #29); Periodic wake up + Wakeup Every (PR #46 — the wake was only an `esp_restart()` that nothing distinguishes from a cold boot, and this build has no outbound client to report with; issue #24 would revive it).

## Dynamic entry templates

PID entries and Custom CAN Filter entries are not static HTML — they're template literals in `main.js` (`addCollapsibleRow` and the filter equivalent). Hidden fields there must be hidden **in the template**, and inputs must keep their class names (`.period-input`, `.class-input`, …) because the store functions query by class.

## Config round-trip gotchas

- `GET /load_config` streams the raw stored `config.json` (legacy configs may lack new keys); `GET /check_status` is the *built* JSON with defaults applied. Don't confuse them.
- `POST /store_config` replaces the whole file and **always reboots** — to change one key: load → modify → post the full object.
- `POST /store_auto_data` writes `auto_pid.json` **without** rebooting; follow with `POST /system_reboot` to apply.
- `csv_grid_hz` is a string key holding `"1".."50"` or `"auto"` (see [csv_logger.md](csv_logger.md)).
