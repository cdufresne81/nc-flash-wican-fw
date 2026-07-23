# Goal Document — No-reboot live-reconfigure (issue #39)

**Repo:** `nc-flash-wican-fw` (WiCAN-PRO, ESP32-S3, ESP-IDF v5.5.3, "NC Flash" fork)
**Base:** `feature/logger-ui-rows` @ `f094cf4` (== v1.8.0 tip on `wican-pro`). Implement on a fresh branch `feature/live-reconfigure` off this tip. `components/fast_log/datalog_stream.c` is absent on this base.
**Closes:** #39 ("Investigate why the device has to reboot when we change a setting").
**Provenance:** designed by a 20-agent design workflow (3 groundings → 4 architectures → 12 adversarial judges → synthesis) and then adversarially re-reviewed by an independent Fable engineer whose MUST-FIX/SHOULD-FIX findings are folded in below (marked ⟢).

> **/goal condition (paste line):**
> `/goal Implement no-reboot live-reconfigure per docs/goal-live-reconfigure.md: (A) config.json Category-B keys apply live via seed-from-live shadow + whitelist memcpy-probe + memcmp reboot-backstop + device_config_file cache refresh, (B) auto_pid.json PID table hot-swaps on the poll task at its safe point with all cross-task UAFs closed (mutex fully hoisted, s_pid_count, file mutex, enabled-only cmd validation, handler JSON guards), (C) CSV interlock refuses/defers the swap during an open trip, (D) both endpoints return {reboot,applied,msg} JSON and the UI stops falsely claiming "Rebooting". All acceptance criteria below hold and idf.py build exits 0.`

---

## 1. Goal & root cause

**Today:** every settings change reboots. Root cause is architectural: both config files are parsed **once, at boot**, into in-RAM state nothing re-reads.
- `config.json` → parsed by `config_server_load_cfg()` (`main/config_server.c:2016`), sole call site at boot (`config_server.c:3035`). `POST /store_config` (`config_server.c:692`) writes the file and reboots (`:844`); it never mutates the in-RAM `device_config` struct (`config_server.c:210`) nor the cached raw-string `device_config_file` (`config_server.c:119`).
- `auto_pid.json` → the PID table (`autopid_config`, `autopid.c:87`) is built once and never rebuilt; `autopid_load_config_only()` is idempotent (`autopid.c:2830`). `POST /store_auto_data` (`config_server.c:1054`) writes the file only (no reboot); its own response says *"take effect after submit."*

**Goal:** apply the changes that are *safe* to apply live, keep the reboot only where genuinely required, and make the UI tell the truth (the Store toast falsely says *"Settings saved successfully. Rebooting…"* at `main.js:1350` even though `/store_auto_data` never reboots).

Two independent mechanisms, one honest HTTP/UI contract.

---

## 2. Design (grounded in the tree)

### A. `config.json` Category-B live-apply — *seed-from-live shadow → whitelist probe → reboot dominates → cache refresh*

Reuse the **real boot parser** so a live apply is byte-for-byte reboot-equivalent (inherits every coercion: led_blink enable/disable coerce `config_server.c:2816`, batt_alert force-disable `:2278`, home_/drive_ defaults `:2489-2597`, sta_fallbacks reset `:2027`).

- **STEP 0 (prereq, mechanical, highest-risk):** extract `static bool config_server_parse_cfg_into(device_config_t *dst, const char *cfg)` from `config_server_load_cfg()` (`config_server.c:2016`). Every `device_config.X` → `dst->X`; the two error labels (`:2850-2882`) **return false** instead of unlink/restore-default/reboot. Boot caller (`:3035`) wraps it: `if(!config_server_parse_cfg_into(&device_config, buf)){ <old restore-default+reboot body> }`. No parse-logic change. ⟢ *Verified as the doc's highest risk — a single missed `device_config.X → dst->X` substitution corrupts the diff; criterion 2 makes the omission grep-detectable.*
- **STEP 1:** in `store_config_handler` keep recv/validate/ap_ssid-check/file-write (`:727–836`). **Do not reboot yet.**
- **STEP 2 (seed-from-live):** `device_config_t *shadow = heap_caps_malloc(sizeof*shadow, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)`; **`memcpy(shadow,&device_config,sizeof*shadow)` BEFORE parsing** (padding/skipped fields become byte-equal to live RAM → diff can't false-positive). Then `if(!config_server_parse_cfg_into(shadow,buf)){ free(shadow); <fall through to today's reboot at :844> }`.
- **STEP 3 (probe + backstop — reboot dominates):** `device_config_t *probe = heap_caps_malloc(...)`; `memcpy(probe,&device_config,sizeof*probe)`. For **every** whitelisted field `W` (changed or not — identical bytes are a no-op): `memcpy((char*)probe+offsetof(...,W), (char*)shadow+offsetof(...,W), sizeof W)`. Then `bool reboot_needed = memcmp(probe, shadow, sizeof*shadow) != 0`. `probe` = live with only-whitelisted fields overwritten by the parsed values; `shadow` = fully parsed. They differ **iff a non-whitelist field changed** → covers every reboot-required field (present *and future*) with zero hand-enumeration. ⟢ *Use full-field `memcpy`, NOT `strlcpy` — the backstop's correctness must not silently depend on the parser staying strlcpy/snprintf-only across future edits. Copying all whitelisted fields (not just "changed" ones) also deletes the separate changed-field bookkeeping.*
- **STEP 4 (reboot path):** if `reboot_needed` → `free(shadow); free(probe);` reboot exactly as today (`config_server.c:844`), touch **no** RAM, respond `{reboot:true, applied:"reboot", msg:"Configuration saved. Rebooting to apply."}`.
- **STEP 5 (no-reboot apply):** for each whitelisted field, `memcpy(&device_config.W, &shadow->W, sizeof device_config.W)` (field-width only — **never** whole-struct memcpy: that rewrites reboot fields and transiently zeroes `sta_fallbacks` at `:2027`). Then do STEP 6. `free(shadow); free(probe); free(buf);` respond `{reboot:false, applied:"live", msg:"Configuration applied (no reboot)."}` without arming the reboot timer.
- **STEP 6 (⟢ MUST-FIX 3 — refresh the raw-string cache):** the no-reboot path MUST also replace the global `device_config_file` (`config_server.c:119`), the cached raw JSON that `/load_config` serves verbatim (`:899-905`) and that the status JSON re-parses (`:3570`). Without this, the UI's post-save page reload (`main.js:2164`) repopulates the form from the **stale** cache and `loadedPassthrough` (`main.js:2094`) re-sends the OLD values, so the **next Submit reverts the live-applied change**. Replace the cache with the freshly written config bytes (the buffer already validated/written in STEP 1). httpd handlers serialize on one server task, so a `char *old = device_config_file; device_config_file = new; free(old);` swap is safe here.
- **STEP 7 (kick):** none — every whitelisted consumer already re-reads: LED task each loop (`led_indicator.c:155`), smartconnect per transition (`smartconnect.c:175/184/627-643`), batt-alert creds/protocol when an alert fires (`sleep_mode.c:471/479`).

**LIVE whitelist (20 keys):** `led_blink`; `home_ssid/home_password/home_security/home_protocol`; `drive_ssid/drive_password/drive_security/drive_protocol/drive_connection_type/drive_mode_timeout`; `batt_alert/batt_alert_protocol/batt_alert_ssid/batt_alert_pass/batt_alert_url/batt_alert_port/batt_alert_topic/batt_mqtt_user/batt_mqtt_pass`. ⟢ *`batt_alert_protocol` added for consistency (read live at alert-fire like the other creds). `batt_alert` (master) is inert — the parser force-disables it (`config_server.c:2278`) so it can never diff; harmless to keep listed, never applied.*

**Deliberately EXCLUDED (stay reboot-required):** `batt_alert_volt`, `batt_alert_time` — cached once into `adc_task` statics (`sleep_mode.c:321-332`), so a RAM update would be a silent no-op; a change to either forces a reboot (safe). Pre-existing inertness, not fixed here.

**Concurrency:** `device_config` is a lock-free file-scope static; ⟢ *confirmed all 20 whitelisted fields are fixed `char[]` (`config_server.h:110-158`) and `device_config_t` contains no pointers* → a torn read is bounded and self-healing (one wrong LED blink toggle / one retried connect), never dangling. Accept eventual consistency (matches existing posture).

### B. `auto_pid.json` PID-table hot-swap — *safe-point ownership + fully-hoisted mutex + file mutex (in-task RCU)*

The swap runs **on the poll task** at its top-of-loop quiescent point (`poll_log.c:452`, right after `can_should_park()`, before the sweep at `:461`, where the task holds no table pointer — ⟢ *loop traced `:438-598`, confirmed no retained table pointer survives across the top of loop; the sweep takes `&s_cfg->pids[i]` only transiently at `:462`*). The httpd handler only sets a flag. Prereqs close the cross-task hazards:

- **P0 (mutex-field-UAF) — ⟢ MUST-FIX 1, corrected:** hoist the config mutex to `static SemaphoreHandle_t s_autopid_mutex` (`autopid.c`); assign at both create sites (`:2844`, `:2945`). Route through it **every** access that today reads `autopid_config->mutex`, which is **not only** `autopid_lock`/`autopid_unlock` (`:235-250`) and the two takes in `autopid_get_config` (`:1126`, `:1132`), **but also the pre-lock NULL-checks** `if(!autopid_config || !autopid_config->mutex)` in `autopid_collect_log_columns` (`autopid.c:273`, CSV-writer task) and `autopid_get_config` (`autopid.c:1119`, httpd task). Those pre-lock derefs run cross-task **before** the lock is held, so if they read the swappable global's `->mutex` a concurrent `deep_free(old)` is a UAF — hoisting only the acquire path does NOT close it. After the fix, **no `autopid_config->mutex` deref exists outside the two create sites** (criterion 9). `new->mutex=old->mutex` stays for compat but is no longer load-bearing.
- **P1 (cross-task `s_cfg` UAF at `poll_log.c:733`):** ⟢ *confirmed* — `/poll_status` (`poll_log_get_status_json`, called on the httpd task `config_server.c:1996`) reads `(unsigned)(s_cfg ? s_cfg->pid_count : 0)` unlocked. Add `static volatile uint32_t s_pid_count`; set it after the first `s_cfg` assign and inside the swap; change `:733` to read `s_pid_count`. After this **no task but the poll task ever dereferences `s_cfg`.**
- **P2 (⟢ MUST-FIX 2 — file-write vs reload TOCTOU → heap overflow):** `load_autopid_config` opens `auto_pid.json` **twice** — once to count (`autopid_config.c:947` → `:324-345`) then once to parse (`:976`), sizing `pids[]` from the count (`:967`). `store_auto_data_handler` truncate-writes the same file on the httpd task (`config_server.c:1145`). With the hot-reload, a second Store while the poll task is between count and parse makes parse iterate the *larger* new arrays and write past the calloc'd bound — a **user-triggerable heap overflow** (double-click Store). Add a file mutex (e.g. `s_autopid_file_lock`) held by BOTH the handler's write (`config_server.c:1145-1164`) and the reload's count+parse (wrap the `load_autopid_config()` call in `autopid_reload_config`). A mid-`fwrite` read then parses a whole old-or-new file — never a torn size.
- **NEW deep-free:** `void autopid_config_deep_free(autopid_config_t*)` (`autopid_config.c`, proto in `autopid.h`) — frees `pids[].cmd/init/rxheader` + each parameter's name/expression/unit/class + arrays, `can_filters[]` params+arrays, `calculated[]` strings, the six top-level `char*`, then `free(c)`. **Guards every array base** (not just `free(NULL)` on strings — closes partial-alloc deref). **Never** `vSemaphoreDelete(c->mutex)`. ⟢ *Ownership confirmed correct: all bases are `heap_caps_calloc`'d (`autopid_config.c:953/967/752/800/698`), all param strings strdup'd incl. `"none"` defaults (`:223-226`) → no static-literal, no aliasing, no double-free. Pre-existing overwrite leaks (std unit/class `:625`, `standard_init` re-assign `:597`) are leak-only.*
- **NEW reload orchestrator:** `autopid_config_t *autopid_reload_config(void)` (`autopid.c`), callable **only from the poll task**:
  1. take `s_autopid_file_lock`; `new = load_autopid_config()` — the RAW parser (`autopid_config.c:941`), **not** the idempotent `autopid_load_config_only()`; parse **off the autopid lock** (SD/flash I/O); give `s_autopid_file_lock`.
  2. **Known-good validation — ⟢ SHOULD-FIX 4, corrected:** reject if `!new`. `cmd==NULL` is a **normal** boot state (car_data pids without a `"pid"` key `:878`, std pids whose name fails to match `:631`, zeroed tail entries `:980`) that the poll loop already tolerates (`poll_log.c:317`) — so do **not** reject on `cmd==NULL` blindly (that would make a live reload reject a table a reboot loads fine, breaking reboot-equivalence). Reject only: any **enabled** pid with `cmd==NULL`, or `(parameters_count>0 && parameters==NULL)`. Decide the empty-table intent explicitly (a legitimately emptied table should be allowed, not force-rejected by `pid_count==0`). On reject: `deep_free(new); return NULL;` (keep the working table — never brick).
  3. `new->mutex = old->mutex;` (compat).
  4. `autopid_lock(portMAX_DELAY)`; if it fails, `deep_free(new); return NULL;`.
  5. `old = autopid_config; autopid_config = new;` (single aligned pointer store under the lock).
  6. `stale = autopid_config_json; autopid_config_json = NULL;` (invalidate `/autopid_data` cache under the lock → lazy rebuild). ⟢ *SHOULD-FIX 8: prefer two-generation retirement — free the PREVIOUS stale string at the NEXT reload — over a pure per-reload leak, since the cache can be tens of KB on this PSRAM-tight device.*
  7. `autopid_unlock();`
  8. `autopid_config_deep_free(old);` (safe — see proof).
  9. `return new;`
- **NEW poll_log plumbing:** `static volatile bool s_reload_requested`; `bool poll_log_request_reload(void){ if(!s_active) return false; s_reload_requested=true; return true; }` (proto in `poll_log.h`; `s_active` doubles as the mode guard → non-POLL_LOG modes return false and stay reboot-required). At the safe point (`poll_log.c:452`):
  ```c
  if (s_reload_requested && !csv_logger_session_active()) {
      s_reload_requested = false;
      autopid_config_t *n = autopid_reload_config();
      if (n) { s_cfg = n; s_pid_count = n->pid_count; s_last_reload_ok = true; }
      else  { s_last_reload_ok = false; ESP_LOGW(TAG,"PID reload rejected — keeping old table"); }
  }
  ```
  Leave the flag **set** while a trip is open (don't clear in the gated-out case) so the reload naturally defers and drains at the first safe point after the session closes. ⟢ *SHOULD-FIX 6: publish `s_last_reload_ok`/last-reload-result in `/poll_status` so a rejected reload is observable rather than silent (the handler answers "live" optimistically before the poll task validates).*

- **Handler-side JSON guards — ⟢ SHOULD-FIX 7, promoted:** the RAW parser has runtime-reachable crashes on hostile-but-valid JSON — `strcmp(NULL)` at `autopid_config.c:439` (`"standard_pids":123`) and `strlen(NULL)` at `:478` (non-string `"PID"`). Today these panic at next boot (recovered by the poll_log crash-guard `poll_log.c:607`); the hot-reload moves the panic **mid-drive on the poll task**. NULL-guard those parser sites (and the related uninitialised-`cmd` path at `:478-483`), or schema-validate the incoming table in `store_auto_data_handler` before accepting it. Prefer NULL-guarding the parser (fixes both the boot and reload panic).

**UAF-free proof (POLL_LOG mode).** ⟢ *Independently re-verified by grepping every `autopid_config`/`s_cfg` deref in the tree.* *R1* poll hot-path unlocked reads (`poll_log.c:462`, `:316-347`, `:257-265`) run on the poll task strictly between safe points; the swap runs AT the safe point → mutually exclusive by construction. *R2* `autopid_eval_calculated_channels` (`autopid.c:552`, called `poll_log.c:476`) is same-task → excluded. *R3* the cross-task readers — `autopid_collect_log_columns` (CSV writer, `autopid.c:268`) and `autopid_get_config` (httpd, `autopid.c:1113`) — each re-read the global inside `autopid_lock` and cache no base across the region; with P0 their **pre-lock** `->mutex` checks no longer touch the swappable global either. *R4* the AUTO_PID-only readers (`autopid_find_standard_pid` `:727/752/919/937`, the two HTTP test handlers, `autopid_pid_validation_enabled`) are all gated by `config_server_protocol()==AUTO_PID` and unreachable in POLL_LOG. *Mutex*: acquisition + pre-checks use `s_autopid_mutex`. */poll_status*: reads `s_pid_count`. *File*: P2 mutex serialises write vs count+parse. `old` is freed after (a) the global is republished under the lock and (b) `s_cfg=new` → no reader holds an old base. Race-free.

**Scope:** `fast_log` has the identical `s_cfg` pattern (`fast_log.c:77/217`) but is a mutually-exclusive boot mode → out of scope. `AUTO_PID` holds table pointers across multi-second ELM reads and mutates `standard_init` in place → no clean safe point → stays reboot-required. The idempotent guard (`autopid.c:2830`) is left untouched.

### C. CSV interlock — *refuse-then-defer around an open trip; swap under `autopid_lock`*

- **Keep the existing insulation unchanged:** ⟢ *confirmed `csv_logger_record` copies row values by value into fixed arrays before `xQueueSend` (`csv_logger.c:802`), so the record queue retains no table pointer.* `csv_cols` stays a private, deep-copied, session-scoped table enumerated only at session-open (`csv_logger.c:450/491`), frozen across rotations (`:774`); the per-row path takes no autopid lock and never touches `autopid_config` (hard rule `csv_logger.c:298`). **Do not** re-point `csv_cols` at `autopid_config` or re-enumerate mid-session.
- **Handler answer (`store_auto_data_handler`, `config_server.c:1054`):** file is always written first (`:1145`, under the P2 file lock) so it applies at next boot regardless. Then `queued = poll_log_request_reload()`: `!queued` → `{reboot:true, applied:"reboot", msg:"PID table saved; takes effect after reboot."}` (the only correct answer in AUTO_PID/FAST_LOG); else `csv_logger_session_active()` → `{reboot:false, applied:"deferred", msg:"Datalog trip in progress — new PID table takes effect when this trip ends (or on reboot)."}`; else `{reboot:false, applied:"live", msg:"PID table applied. New logging columns start with the next trip."}`.
- **Race-safe gate:** the in-task re-check `!csv_logger_session_active()` (`csv_logger.c:820`, plain aligned-bool read) adjacent to the swap closes the handler-side TOCTOU; the swap also holds `autopid_lock` while the provider takes `autopid_lock(100)`, so the residual session-open-during-swap edge is — **once P0 removes the provider's pre-lock global deref** — a benign one-trip fidelity issue (that trip's columns frozen from the old table), never a crash.

### D. HTTP / UI contract

Both endpoints return `{"reboot":bool, "applied":"live|reboot|deferred|error", "msg":str}` instead of bare text. `main/web/src/main.js` is embedded verbatim into the firmware (edit it); `main/web/homepage.html` is **generated** — regenerate via the existing web build step, never hand-edit.
- **Store** (`storeAutoTableData`, `main.js:1228`; toast `:1350`): parse the reply, render `msg` verbatim; **remove** the false "Rebooting…" string; don't disable buttons on a false reboot premise.
- **Submit** (`postConfig`, `main.js:2054`; `onreadystatechange` `:2161`): ⟢ *confirmed `postConfig` POSTs `/store_auto_data` first (`:2058`) then `/store_config` (`:2159`)* — parse the `/store_config` envelope: `reboot==true` keeps today's countdown/reconnect UX; `reboot==false` toasts `msg`, **stays connected** (STEP 6 keeps `/load_config` fresh so a form refresh is now correct), re-enables `submit_button`, skips the countdown.
- **Back-compat:** wrap `JSON.parse` in try/catch; on failure treat the body as legacy plain text and assume reboot, so UI and firmware can ship independently.
- **Do not** change element IDs (`custom_pid_store`, `submit_button`), classes, or fn names (`storeAutoTableData`, `postConfig`).

---

## 3. Acceptance criteria (each verifiable from the session transcript)

Each check is a `grep`/`git diff`/`idf.py build` the implementing session runs and shows in its own output.

1. **Parser extracted, non-destructive:** `grep -n config_server_parse_cfg_into main/config_server.c` shows a `static bool` def; its body shows the error labels returning `false` with no `unlink()`/`esp_restart()` inside; the sole boot caller (~`:3035`) shows the restore-default+reboot wrapper.
2. **⟢ STEP-0 equivalence (missed-substitution guard):** `grep -n "device_config\." ` restricted to the body of `config_server_parse_cfg_into` returns **zero** hits — every field access is `dst->` (any remaining `device_config.` is the exact bug the doc's highest risk warns about).
3. **Shadow seeded before parse:** `grep -n memcpy` around the shadow alloc shows `memcpy(shadow,&device_config,…)` **before** `config_server_parse_cfg_into(shadow,…)`.
4. **Probe backstop + minimal apply:** `grep -n memcmp` in `store_config_handler` shows `memcmp(probe,shadow,sizeof …)`; the probe is built with full-field `memcpy` (no `strlcpy`); the no-reboot apply copies whitelist fields into `device_config` field-by-field with **no** whole-struct memcpy.
5. **⟢ Cache refreshed on live-apply (MUST-FIX 3):** `grep -n device_config_file` shows the no-reboot path replaces the `device_config_file` global (swap+free) before responding; the reboot path does not.
6. **Whitelist exact:** the whitelist contains exactly the 20 keys (incl. `batt_alert_protocol`) and **no** `batt_alert_volt`/`batt_alert_time`.
7. **Deep-free guards every base:** `grep -n autopid_config_deep_free` shows the def in `autopid_config.c` + decl in `autopid.h`; body shows `if(c->pids)`/`if(pids[i].parameters)`/`if(c->can_filters)`/`if(c->calculated)` guards and **no** `vSemaphoreDelete`.
8. **Reload validates + preserves mutex + orders swap/free:** `grep -n autopid_reload_config` shows the def; body shows reject on `!new`, the **enabled-pid-only** `cmd==NULL` (and `parameters_count>0 && parameters==NULL`) reject, `new->mutex=old->mutex`, `autopid_lock(portMAX_DELAY)` before the swap, and `autopid_config_deep_free(old)` **after** `autopid_unlock()`.
9. **⟢ Mutex FULLY hoisted (MUST-FIX 1):** `grep -n "autopid_config->mutex" components/autopid/autopid.c` shows hits **only** at the two create sites (`:2844`,`:2945`) — zero elsewhere; `autopid_lock`/`autopid_unlock` and both pre-lock NULL-checks (`autopid_collect_log_columns` `:273`, `autopid_get_config` `:1119`) test `s_autopid_mutex`.
10. **Status UAF closed:** `grep -n s_pid_count components/fast_log/poll_log.c` shows the volatile + set sites; `poll_log_get_status_json` reads `s_pid_count` and no `s_cfg->pid_count` remains in that function.
11. **⟢ File-write/reload mutex (MUST-FIX 2):** `grep` shows one shared lock taken by both `store_auto_data_handler`'s file write and `autopid_reload_config`'s `load_autopid_config()` call.
12. **Swap on poll task, gated:** `grep -n poll_log_request_reload` shows the setter only sets the flag; the drain block in `polllog_rx_task` sits after `can_should_park()` and before the sweep, gated on `!csv_logger_session_active()`, calling `autopid_reload_config()` and updating `s_cfg`+`s_pid_count`.
13. **⟢ Parser NULL-guards (SHOULD-FIX 7):** `grep` around `autopid_config.c:439/478` shows NULL/type guards so a non-string `"standard_pids"`/`"PID"` can no longer `strcmp(NULL)`/`strlen(NULL)`.
14. **JSON envelope on both endpoints incl. fallback:** `grep -n applied` in `store_config_handler` and `store_auto_data_handler` shows a `{"reboot":…,"applied":…,"msg":…}` body per path; the `store_auto_data` `reboot`(¬queued)/`deferred`/`live` branches all exist.
15. **UI honest + parses envelope:** `grep` around `main.js:1350` shows "Rebooting" removed from the `/store_auto_data` success path; `postConfig` shows `JSON.parse` in try/catch branching on `result.reboot` with a plain-text fallback; IDs `custom_pid_store`/`submit_button` and fn names `storeAutoTableData`/`postConfig` unchanged.
16. **Builds clean:** `idf.py build` exits 0 (ESP-IDF v5.5.3 + Python 3.10 PATH workaround); no undefined-reference to `autopid_config_deep_free`/`autopid_reload_config`/`poll_log_request_reload`.
17. **Idempotent guard untouched:** `git diff` of the `autopid.c:2828-2832` hunk (the `return existing autopid_config` guard) is empty.

---

## 4. Constraints (must NOT change)

- **No** `config.json`/`auto_pid.json` schema or key-name changes — live-apply must be reboot-equivalent by reusing the existing parser.
- **No** hand-edit of `main/web/homepage.html` (generated) — regenerate via the web build step.
- **No** change to UI element IDs, CSS classes, or JS fn names — only response handling and toast strings.
- **No** change to existing reboot-on-structural-change behavior: any changed field outside the LIVE whitelist MUST still reboot; the `memcmp(probe,shadow)` backstop dominates.
- **No** touching `autopid_load_config_only()`'s idempotent guard (`autopid.c:2830`).
- **No** `vSemaphoreDelete` of the autopid config mutex anywhere; and **no** `autopid_config->mutex` deref outside the two create sites.
- The PID swap MUST run on the poll task at its safe point — the handler may only set a flag; **never** swap from httpd/CSV-writer.
- **No** re-pointing `csv_cols` at `autopid_config` or mid-session re-enumeration — no mid-trip column desync.
- The safe-point drain MUST sit AFTER `can_should_park()` so the brick-safe single-CAN-owner interlock is unchanged.
- **No** raw-copy of `batt_alert` enable or `batt_alert_volt`/`batt_alert_time` into RAM — keep them reboot-required.
- The live-apply path MUST refresh `device_config_file` (so `/load_config` and a Submit don't revert it) but MUST NOT whole-struct-memcpy `device_config`.
- Preserve the plain-text legacy fallback in the UI so mismatched firmware/UI versions don't hang.

---

## 5. Manual verification checklist (OUTSIDE the /goal condition — needs hardware/eyes)

Run on the bench OBD-PRO after OTA (see MEMORY `wican-test-device` for the address/flow):
- [ ] `POST /store_config` changing **only** `led_blink` → device does **not** reboot (connection stays up); the activity LED switches between blinking and solid within one indicator tick.
- [ ] After that live apply, **reload the page and click Submit without changing anything** → the led value is **retained** (proves the `device_config_file` cache refresh; regression guard for MUST-FIX 3).
- [ ] `POST /store_config` changing a reboot key (e.g. `can_datarate`) → device **does** reboot; change applied after reconnect.
- [ ] Engine OFF / no trip: `POST /store_auto_data` with an edited table → reply `"live"`, no reboot; the **next** trip's CSV wide header reflects the new channels.
- [ ] Engine ON / trip open: `POST /store_auto_data` → reply `"deferred"`; current trip's columns **unchanged** mid-trip; stop the trip → new table live on the next trip, no reboot.
- [ ] **Double-click Store rapidly** (engine off) and hammer `GET /poll_status` in a loop → no crash/reset; pid count updates (exercises the P2 file-mutex + swap/free path).
- [ ] `POST /store_auto_data` with a deliberately malformed table (`"standard_pids":123`, non-string `"PID"`) → rejected cleanly, **no panic/reboot mid-run**, old table kept, `/poll_status` shows the reject (SHOULD-FIX 6/7).
- [ ] `/autopid_data` reflects the new table after a hot reload (may need one extra request to trigger the lazy rebuild).
- [ ] Web UI Store/Submit show honest toasts (never "Rebooting" on a no-reboot save); Submit stays connected when only live keys changed.

---

## 6. Open risks (for the human to weigh)

- **Torn reads** on lock-free `device_config` during live-apply: bounded, array-safe, self-healing (never dangling). Escalate to a config-apply mutex only if unacceptable.
- **PSRAM double-alloc** during PID reload holds old+new tables briefly on a device the CSV-trip-OOM note flags as tight — a legit new table could be silently rejected under memory pressure (logged loudly; `!session_active` gate reduces contention).
- **Partial-alloc hardening** depends on `load_autopid_config` leaving detectable NULLs — keep the enabled-pid validation in lockstep with the parser's allocation set.
- **Optimistic "live"** answer for the PID store: a session opening in the sub-second gap makes the poll task defer *after* the handler said "live" (benign; `s_last_reload_ok` in `/poll_status` makes the true outcome observable).
- **STEP 0** extracts a large (~830-line) boot-critical parser — criterion 2 guards the missed-substitution failure mode, but still do a boot smoke test.
- **Response-body contract change** to JSON may break any external client that substring-matched the old "Rebooting" text — confirm no SLCAN/host tooling depends on it.
- **Branch topology:** when this later merges onto `feature/live-datalog-stream`, re-verify the stream's `csv_cols` mirror needs no extra invalidation and re-check line anchors.
- **Pre-existing bugs surfaced but deliberately not fixed here:** `batt_alert` master force-disabled at parse (`config_server.c:2278`); `batt_alert_volt/time` cached in `adc_task`; parser overwrite leaks (`autopid_config.c:597/625`). Flagged for separate follow-up, not in scope.
