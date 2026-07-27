# Firmware internals

Deep-dive notes on how this fork works under the hood — written for whoever touches the code next (human or AI session). The upstream user-facing docs site lives in `docs/content/`; this directory is for **developer/architecture** documentation.

Ground rules for these docs:

- Cite **symbols and constants**, not bare line numbers (lines drift; `grep` the symbol).
- When behavior was verified on a real device, say so and with which build.
- If a doc contradicts the code, the code wins — fix the doc in the same PR.

## Contents

| Doc | Covers |
|---|---|
| **[architecture.md](architecture.md)** | **Start here.** The whole-system map: the fork/upstream layering, boot order, task topology, the two paths to the CAN bus, the bus interlock, config model, extension seams, invariants, and how to build/flash/verify |
| [audit-2026-07.md](audit-2026-07.md) | Dated snapshot (July 2026): verified defects, dead weight with sizes, the refactor that pays for itself, sequencing, and an explicit *what not to do* list |
| [rewrite-vs-evolve-2026-07.md](rewrite-vs-evolve-2026-07.md) | Decision brief: keep evolving, trim, rebuild, or extract-then-grow-v2-beside-v1 — provenance, flash/RAM budget, four costed paths, an adversarial review that overturned the first recommendation, and what evidence would change the current one |
| [poll_log.md](poll_log.md) | The Datalogger protocol: free-running PID polling, hybrid broadcast capture, engine-off quiesce, sweep-rate measurement, `/poll_status` |
| [csv_logger.md](csv_logger.md) | The wide-CSV trip logger: the fixed-rate grid, the Auto (fastest) rate, registration patterns, RTC crash guard |
| [web_ui.md](web_ui.md) | Web UI build pipeline, lint gates, and the hidden-feature (streamline) pattern |

## Not yet written

[architecture.md](architecture.md) now gives every subsystem at least a map
reference and covers the coexistence protocol, boot, config semantics and the
build/release pipeline at overview depth. Still wanted as **dedicated** deep
dives, tracked in **issue #30**: the NC Flash fast-read/fast-write codecs,
event_log + crash_report, the LED state machine, and the Wi-Fi manager's
mode/reconnect behaviour. Add new docs here and update this table.
