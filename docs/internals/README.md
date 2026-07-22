# Firmware internals

Deep-dive notes on how this fork works under the hood — written for whoever touches the code next (human or AI session). The upstream user-facing docs site lives in `docs/content/`; this directory is for **developer/architecture** documentation.

Ground rules for these docs:

- Cite **symbols and constants**, not bare line numbers (lines drift; `grep` the symbol).
- When behavior was verified on a real device, say so and with which build.
- If a doc contradicts the code, the code wins — fix the doc in the same PR.

## Contents

| Doc | Covers |
|---|---|
| [poll_log.md](poll_log.md) | The Datalogger protocol: free-running PID polling, hybrid broadcast capture, engine-off quiesce, sweep-rate measurement, `/poll_status` |
| [csv_logger.md](csv_logger.md) | The wide-CSV trip logger: the fixed-rate grid, the Auto (fastest) rate, registration patterns, RTC crash guard |
| [web_ui.md](web_ui.md) | Web UI build pipeline, lint gates, and the hidden-feature (streamline) pattern |

## Not yet written

The coverage roadmap — including the NC Flash ↔ WiCAN flashing/coexistence protocol, event_log, LED subsystem, Wi-Fi manager, config_server semantics, OTA/release pipeline and more — is tracked in **issue #30**. Add new docs here and update this table.
