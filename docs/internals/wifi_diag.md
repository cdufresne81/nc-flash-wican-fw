# Wi-Fi diagnostics — what the device records about its own link

Issue #105. Users report two symptoms in station mode — transfers that are slow, and connections
that drop — and before this component the firmware kept no evidence of either.

The reason that mattered most: the IDF hands us a disconnect reason code on every drop, and
`wifi_mgr.c` logged it with `ESP_LOGW` and threw it away. This build compiles at
`CONFIG_LOG_MAXIMUM_LEVEL=INFO`, and the board's USB-C port is a USB *host* at runtime, so there is
no serial console anyone can read. By the time a user noticed, the single most useful fact about the
failure was gone.

`components/wifi_diag/` keeps that evidence and presents it three ways: a live page, a paste-ready
report, and a persistent trail that survives reboots.

## The two data sources, and why both

The symptoms fail differently, so they are measured differently.

| Source | What it produces | Lives in |
|---|---|---|
| 1 Hz sampler task (`wd_sampler_task`) | RSSI, channel, PHY, power-save mode, AP/STA channel clash, internal-heap headroom → rolling aggregates + a 120-sample RSSI ring | RAM only |
| Event hooks (`wifi_diag_note_*`) | every association, IP lease, disconnect-with-reason, SSID ban | RAM ring **and** the SD event log |

A single spot reading proves nothing — *"RSSI averaged −78 dBm over twenty minutes"* is a diagnosis
and *"RSSI is −78 dBm"* is not. Equally, the aggregates alone cannot explain a drop; only the reason
code can, and only if it was captured at the moment it happened.

The RSSI recorded against a disconnect is the sampler's **last reading while the link was up**.
`esp_wifi_sta_get_ap_info()` fails the instant the link drops, so reading it inside the disconnect
handler would report nothing at all. This is the main reason the two halves are coupled rather than
independent.

## Persistence is delegated, not reimplemented

Milestone lines are emitted as `EVL_WIFI` through [event_log](../../components/event_log/include/event_log.h),
which already ships a brick-safe SD writer, an RTC crash guard, rotation, and a retrieval endpoint.
Reimplementing that would have meant a second writer task and a second crash guard for the same job.

It also puts Wi-Fi drops in the **same timeline** as boot, sleep and datalog events, which is what
the drop-while-driving case needs — nobody is holding a browser open in a moving car, and the
question is almost always *"what else was happening when it dropped?"*

The 1 Hz samples are deliberately **not** persisted. At ~86k lines a day they would rotate the rest
of the log away, and their whole value is in the aggregate.

### Rate limiting

A device stuck in a reconnect loop can drop every few seconds. `WD_EVL_REPEAT_MS` (60 s) throttles
only the *shared log line* for a repeating identical reason. The RAM ring and the per-reason tally
still record **every** drop, and the suppressed count rides along on the next line that does get
through (`[+N similar suppressed]`), so nothing is silently lost.

## Findings — the part that makes it a tool

`wd_findings()` reads the aggregates against a fixed rule set and states what was observed *and what
it means for the symptom*. This is what separates the page from a data dump. The rules are
deliberately conservative: a missed diagnosis is recoverable, a confident wrong one sends someone
off for a day.

The rules cover power save, AP/STA channel clash, average RSSI, PHY rate (802.11b-only, no 802.11n),
a dominant disconnect reason, internal-RAM fragmentation, link-uptime percentage, and slow connects.

Two of them exist because the code already told us where to look:

- **Power save.** `wifi_network.c` carries a comment stating that the SmartConnect path never calls
  `esp_wifi_set_ps()` and so runs on the IDF station default (`WIFI_PS_MIN_MODEM`), while manual
  setup gets `WIFI_PS_NONE` via `wifi_mgr_init()`. Same hardware, same router, two different
  power-save regimes depending on how the user set Wi-Fi up. `wd_findings()` reports the mode the
  radio is *actually* in, read back with `esp_wifi_get_ps()`.
- **AP/STA channel clash.** The device runs AP+STA by default. One radio cannot hold two channels;
  it time-slices. `wifi_mgr.h` already defines `WIFI_STA_AP_OVERLAP_BIT` for this condition — the
  sampler measures how much of the time it is actually true.

The TCP window (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT`, 5744 bytes = 4×MSS, the IDF default) is reported
as a **fact in the report's stack-configuration section, not as a finding**. It is not a fault, but
it is a hard ceiling on single-stream throughput — roughly window ÷ round-trip-time, about 2 Mbit/s
at 20 ms RTT — and it is the first thing to check when a transfer is slow while signal, channel and
power save all look healthy. Findings stay observations; this is a constant.

## Routes

All four are registered by `wifi_diag_register_handlers()`, called from `config_server.c` **before**
the catch-all wildcard handler (order is load-bearing — the wildcard would swallow them).

| Route | Serves |
|---|---|
| `/wifi_diag` | JSON: snapshot, aggregates, tally, findings, event ring. Feeds the page. |
| `/wifi_diag/page` | The standalone diagnostic page |
| `/wifi_diag/report` | Plain-text investigation report (`?raw=1` to unmask) |

### Masking

The **report masks** the SSID (first and last character, plus the true length — a length mismatch is
how a trailing-space or homoglyph SSID typo gets spotted) and the tail of the BSSID (the OUI stays:
it identifies the AP vendor, which is diagnostic, and is not specific to a household). Its entire
purpose is to be pasted into a public issue.

The **JSON does not mask**. It renders on the user's own device showing their own network, and
masking there would defeat the most common use — spotting that it joined the wrong SSID.

The pre-shared key is never read by this component on any path, so no route can leak it.

## The page

`main/web/wifi_diag.html` is a standalone page, embedded verbatim via `EMBED_FILES` exactly like
`safemode.html`. It is **not** part of the `build_web.py` minify pipeline and **not** scanned by
`lint_web.py` — both own only the single-page app (`homepage_full.html` + `src/main.js`). Edit it
directly; there is no generated counterpart to keep in step.

A separate page rather than a modal, for three reasons that all matter here:

1. The browser's own reload works (there is also an explicit Refresh button and a 5 s auto-refresh).
2. The URL can be bookmarked, or opened on a phone while someone else drives.
3. A page that must stay readable while the Wi-Fi link is failing should not depend on the rest of
   the app having loaded.

When a fetch fails, the page **keeps the last good values on screen** and says they are frozen,
rather than blanking. A failure to load is itself a data point, and blanking at the moment of failure
would destroy the evidence the user opened the page to collect.

The entry point is a button on the app's Status tab (`openWifiDiag()` in `main.js`), which opens the
page in a new tab and falls back to in-place navigation if a popup blocker returns null.

## Tests

`tools/webtest/wifi_diag.test.mjs` evaluates the page's inline script in a `vm` sandbox. Beyond the
formatters, it pins two **cross-file** invariants that no reviewer would catch by eye across a `.c`
and an `.html`:

- the page's `rssiClass()` colour boundaries line up with `wd_hist_bucket()`'s histogram edges, so
  the page cannot paint a reading green while the report from the same device calls it weak;
- the low-memory threshold (32 KB largest free block) is the same number in both places.

## Gotchas

- `wifi_diag` is a **leaf**: it requires only base IDF plus `event_log`. `main` depends on
  components, never the reverse, so the firmware version and the embedded page bytes are *injected*
  (`wifi_diag_set_fw_version()`, `wifi_diag_set_page()`) rather than read.
- The version comes from `dev_status_get_running_app_info()`, **not** the file-static
  `firmware_version[]` in `main.c` — nothing ever assigns that array (it reaches `wc_mdns_init()` as
  an empty string), so a report built from it would carry a blank build.
- `wifi_mgr_set_attempted_ssid()` is the single choke point where `last_attempted_ssid` is set on a
  path about to call `esp_wifi_connect()`. Four sites used to open-code the same `snprintf`. Route
  any fifth one through it or the connect-time stopwatch will silently miss that path.
- `heap_caps_get_largest_free_block()` walks the heap free lists under the heap lock. At 1 Hz on a
  priority-2 task that is negligible — but it is why the sampler must never move onto a hot path.
- Everything here covers the **current uptime only**. The reboot-surviving record is in the event
  log: `GET /event_log`, look for `WIFI` lines.
