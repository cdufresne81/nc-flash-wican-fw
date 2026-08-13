// Check OUR fork's releases for OTA updates (never the stock meatpiHQ repo, whose
// images would overwrite this custom build). Compares the running firmware against
// the latest v* release tag and, when newer, links straight to that release's OTA
// app image (the *.bin you POST to /upload/ota.bin).
const FW_UPDATE_REPO = 'cdufresne81/nc-flash-wican-fw';
const FW_RELEASES_URL = `https://github.com/${FW_UPDATE_REPO}/releases`;

// Firmware log-rate ceiling, mirrored from WICAN_LOG_MAX_HZ / WICAN_LOG_MIN_PERIOD_MS in
// main/config_server.h (C can't reach JS). Single JS source for the grid-Hz validator and the
// "grid will clamp" warning below -- keep in sync with the C header on any change.
const WICAN_LOG_MAX_HZ = 100;
const WICAN_LOG_MIN_PERIOD_MS = 10;

// Check once per page load. checkFirmwareUpdate() is called from the shared
// /check_status onload handler;
// without this guard the rate-limited (60/hr) GitHub releases API would be
// re-hit each time, for a result that can't change within a page's lifetime.
let fwUpdateChecked = false;
async function checkFirmwareUpdate() {
        if (fwUpdateChecked) return;
        fwUpdateChecked = true;
        try {
            // The About page's fw_version element shows the git-describe tag (e.g.
            // "v1.2.3"), which matches our release tag format exactly. Only trust it
            // when it actually looks like a vX.Y[.Z] tag -- dev builds report a bare
            // SHA (e.g. "b79549b-dirty") that must not be mis-parsed as a version.
            const fwRaw = document.getElementById('fw_version')?.textContent?.trim();
            if (!fwRaw || !/v?\d+\.\d+/i.test(fwRaw)) return;

            // Helpers: extract numeric version and compare a.b.c parts
            const extractVersion = (str) => {
                if (!str) return null;
                const m = String(str).match(/(\d+)(?:\.(\d+))?(?:\.(\d+))?/);
                return m ? [m[1], m[2] || '0', m[3] || '0'].join('.') : null;
            };
            const cmpVersions = (a, b) => {
                const ap = a.split('.').map(n => parseInt(n, 10) || 0);
                const bp = b.split('.').map(n => parseInt(n, 10) || 0);
                const len = Math.max(ap.length, bp.length);
                for (let i = 0; i < len; i++) {
                    const ai = ap[i] || 0;
                    const bi = bp[i] || 0;
                    if (ai > bi) return 1;
                    if (ai < bi) return -1;
                }
                return 0;
            };

            const currentVersion = extractVersion(fwRaw);
            if (!currentVersion) return;

            const response = await fetch(`https://api.github.com/repos/${FW_UPDATE_REPO}/releases`);
            if (!response.ok) return;
            const releases = await response.json();
            if (!Array.isArray(releases)) return;

            // Pick the highest published v* release (skip drafts/prereleases). Don't
            // rely on API ordering -- compare semver across all candidates.
            let latest = null;
            let latestVersion = null;
            for (const rel of releases) {
                if (!rel || rel.draft || rel.prerelease) continue;
                const tag = rel.tag_name || rel.name || '';
                if (!/^v?\d+\.\d+/i.test(tag)) continue;
                const ver = extractVersion(tag);
                if (!ver) continue;
                if (!latestVersion || cmpVersions(ver, latestVersion) === 1) {
                    latest = rel;
                    latestVersion = ver;
                }
            }
            if (!latest) return;

            // Only notify if latest > current
            if (cmpVersions(latestVersion, currentVersion) === 1) {
                const notice = document.getElementById('firmware-update-notice');
                if (notice) {
                    // Prefer a direct link to the OTA app image asset (the obd_pro *.bin
                    // flashable via /upload/ota.bin), not the bootloader/partition-table/
                    // ota_data bins or the source archives. Fall back to the release page.
                    const assets = Array.isArray(latest.assets) ? latest.assets : [];
                    const otaAsset = assets.find(a => {
                        const name = (a && a.name) || '';
                        return /\.bin$/i.test(name) &&
                            /obd[_-]?pro/i.test(name) &&
                            !/bootloader|partition|ota[_-]?data/i.test(name);
                    });
                    const url = (otaAsset && otaAsset.browser_download_url)
                        || latest.html_url || FW_RELEASES_URL;
                    const versionText = ` <span style='color:#b45309'>(v${latestVersion})</span>`;
                    notice.innerHTML = `<span style=\"font-weight: 600;\">New firmware available!</span><br><a id=\"firmware-update-link\" href=\"${url}\" target=\"_blank\" style=\"color: #2563eb; text-decoration: underline;\">Download</a>${versionText}`;
                    notice.style.display = 'block';
                }
            }
        } catch (e) {
            // Silent fail to avoid impacting UI if GitHub is unreachable
        }
    }
    // document.addEventListener('DOMContentLoaded', checkFirmwareUpdate);
    document.addEventListener('DOMContentLoaded', (event) => {
        document.getElementById("submit_button").disabled = true;
        setRTCTime();
    });
    function setRTCTime() {
        const now = new Date();
        
        function decToBcd(val) {
            return Math.floor(val / 10) * 16 + (val % 10);
        }
        
        const rtcData = {
            command: "set_rtc_time",
            hour: decToBcd(now.getUTCHours()),
            min: decToBcd(now.getUTCMinutes()),
            sec: decToBcd(now.getUTCSeconds()),
            year: decToBcd(now.getUTCFullYear() % 100),
            month: decToBcd(now.getUTCMonth() + 1),
            day: decToBcd(now.getUTCDate()),
            weekday: decToBcd(now.getUTCDay()) 
        };
        
        fetch('/system_commands', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(rtcData, null, 0)
        })
        .then(response => response.text())
        .then(data => {
            console.log('RTC time set successfully (UTC):', data);
        })
        .catch(error => {
            console.error('Error setting RTC time:', error);
        });
    }

    function showNotification(message, color = "red", duration = 5000) {
        const notification = document.getElementById("notification");
        
        switch (color) {
            case "red":
                notification.style.backgroundColor = "#fee2e2";
                notification.style.borderColor = "#ef4444";
                notification.style.color = "#991b1b";
                break;
            case "green":
                notification.style.backgroundColor = "#dcfce7";
                notification.style.borderColor = "#22c55e";
                notification.style.color = "#166534";
                break;
            case "blue":
                notification.style.backgroundColor = "#dbeafe";
                notification.style.borderColor = "#3b82f6";
                notification.style.color = "#1e40af";
                break;
            case "yellow":
                notification.style.backgroundColor = "#fef9c3";
                notification.style.borderColor = "#eab308";
                notification.style.color = "#854d0e";
                break;
            default:
                notification.style.backgroundColor = "#fee2e2";
                notification.style.borderColor = "#ef4444";
                notification.style.color = "#991b1b";
        }

        notification.innerHTML = message;
        notification.classList.add("show");
        
        if (window.notificationTimeout) {
            clearTimeout(window.notificationTimeout);
        }
        
        window.notificationTimeout = setTimeout(() => {
            notification.classList.remove("show");
        }, duration);
    }

    async function downloadRestartHistoryJson() {
        try {
            const response = await fetch('/restart_tracker/history');
            if (!response.ok) {
                throw new Error(`HTTP ${response.status}`);
            }

            const data = await response.json();
            const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
            downloadTextFile(`restart-history-${timestamp}.json`, JSON.stringify(data, null, 2));

            showNotification('Restart history downloaded.', 'blue', 3000);
        } catch (error) {
            console.error('Failed to download restart history:', error);
            showNotification(`Unable to fetch restart history JSON. ${error.message}`, 'red');
        }
    }

    function formatRestartTrackerValue(value, fallback = 'N/A') {
        if (value === undefined || value === null || value === '') {
            return fallback;
        }

        return String(value).replace(/_/g, ' ');
    }

    function formatRestartTrackerLocalTime(unixTimestamp, fallback = 'N/A') {
        if (unixTimestamp === undefined || unixTimestamp === null || Number(unixTimestamp) <= 0) {
            return fallback;
        }

        const timestampMs = Number(unixTimestamp) * 1000;
        const date = new Date(timestampMs);
        if (Number.isNaN(date.getTime())) {
            return fallback;
        }

        return date.toLocaleString();
    }

    function toggleApStationWarning() {
        const wifiModeEl = document.getElementById("wifi_mode");
        const apAutoDisableEl = document.getElementById("ap_auto_disable");
        const div = document.getElementById("apstation_warning_div");
        if (!wifiModeEl || !apAutoDisableEl || !div) return;

        const shouldShow = (wifiModeEl.value === "APStation") && (apAutoDisableEl.value === "disable");
        div.style.display = shouldShow ? "block" : "none";
    }


    function renderFallbackNetworks(list) {
        const container = document.getElementById('fallback_rows');
        if (!container) return;
        container.innerHTML = '';
        const limited = Array.isArray(list) ? list.slice(0,5) : [];
        limited.forEach(item => addFallbackNetworkRow(item));
        updateAddFallbackButtonState();
    }

    function addFallbackNetworkRow(data = {}) {
        const container = document.getElementById('fallback_rows');
        if (!container) return;
        const current = container.querySelectorAll('.fallback-row').length;
        if (current >= 5) return;

        const row = document.createElement('div');
        row.className = 'fallback-row';
        row.style.display = 'grid';
        row.style.gridTemplateColumns = '1fr 1fr 120px auto';
        row.style.gap = '8px';
        row.style.margin = '6px 0';

        row.innerHTML = `
            <input type="text" class="fb-ssid" placeholder="SSID" value="${(data.ssid||'').replace(/"/g,'&quot;')}" oninput="submit_enable();" />
            <input type="text" class="fb-pass" placeholder="Password" value="${(data.pass||data.password||'').replace(/"/g,'&quot;')}" oninput="submit_enable();" />
            <select class="fb-sec" onchange="submit_enable();">
                <option value="wpa3" ${((data.security||'wpa3')==='wpa3')?'selected':''}>WPA3</option>
                <option value="wpa2" ${((data.security||'wpa3')==='wpa2')?'selected':''}>WPA2</option>
            </select>
            <button type="button" class="fb-remove" onclick="removeFallbackRow(this)">Remove</button>
        `;
        container.appendChild(row);
        updateAddFallbackButtonState();
        submit_enable();
    }

    function removeFallbackRow(btn) {
        const row = btn.closest('.fallback-row');
        if (row) row.remove();
        updateAddFallbackButtonState();
        submit_enable();
    }

    function updateAddFallbackButtonState() {
        const addBtn = document.getElementById('add_fallback_button');
        if (!addBtn) return;
        const count = document.querySelectorAll('#fallback_rows .fallback-row').length;
        addBtn.disabled = count >= 5;
    }

    function addRowAutoTable() {
        promoteNewEntry(addCollapsibleRow());
        enableAutoStoreButton();
    }

const pidEntryStyles = `
    .pid-entry,
    .custom-canfilter-entry,
    .calculated-entry {
        border: 1px solid #e2e8f0;
        background: #fff;
        border-radius: 6px;
        margin-bottom: 8px;
    }

    .pid-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        cursor: pointer;
        padding: 6px 8px;
        background: #f1f5f9;
        border-radius: 6px 6px 0 0;
        margin: 0;
    }

    .header-left {
        display: flex;
        align-items: center;
        gap: 8px;
        flex: 1;
        min-width: 0;
    }

    .pid-title {
        font-weight: 600;
        font-size: 0.8rem;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
        flex: 1;
    }
    
    .header-right {
        display: flex;
        gap: 0.5rem;
        align-items: center;
    }
    
    .collapse-btn {
        border: none;
        background: transparent;
        font-size: 0.75rem;
        cursor: pointer;
        padding: 2px 4px;
        color: #334155;
    }

    .drag-handle {
        border: none;
        background: transparent;
        font-size: 0.75rem;
        letter-spacing: -2px;
        cursor: grab;
        padding: 2px 6px 2px 2px;
        color: #64748b;
        touch-action: none;   /* the handle owns the touch gesture; without this the page scrolls instead of dragging */
        user-select: none;
        -webkit-user-select: none;
    }

    .dragging {
        opacity: 0.9;
        box-shadow: 0 6px 16px rgba(15, 23, 42, 0.25);
        position: relative;
        z-index: 10;
    }

    .dragging .drag-handle {
        cursor: grabbing;
    }

    .pid-content {
        padding: 8px 10px;
    }
    
    .pid-content.hidden {
        display: none;
    }

    .delete-btn {
        background: #dc2626;
        color: #fff;
        border: none;
        padding: 4px 8px;
        border-radius: 4px;
        cursor: pointer;
        font-size: 0.65rem;
        margin-left: 12px;
    }

    .test-btn {
        background: #2563eb;
        color: #fff;
        border: none;
        padding: 4px 8px;
        border-radius: 4px;
        cursor: pointer;
        font-size: 0.65rem;
    }

    .test-result {
        font-size: 0.85rem;
        max-width: 220px;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .sample-every-hint {
        margin-left: 8px;
        color: #6b7280;
        font-size: 12px;
        white-space: nowrap;
    }

    /* Muted legend under the broadcast Expression box (.expr-hint): the raw-CAN byte-naming note. */
    .expr-hint {
        display: block;
        margin-top: 4px;
        color: #6b7280;
        font-size: 12px;
        line-height: 1.4;
    }
`;

// Test results are shown two ways: a compact colored chip on the row (a persistent
// pass/fail marker) and the shared toast (showNotification) carrying the full
// message — the chip is only wide enough for a word and would ellipsize a real
// error, which was the "doesn't display the message fully" bug.
function showTestOutcome(resultEl, ok, chipText, detail, toastColor) {
    if (resultEl) {
        // A yellow toast means "can't test right now" (trip running, engine off) rather than a hard
        // failure — give the on-row chip a matching amber state instead of the red error chip.
        const warn = !ok && toastColor === 'yellow';
        resultEl.style.display = 'inline-flex';
        resultEl.classList.add('status-indicator');
        resultEl.classList.toggle('status-connected', ok);
        resultEl.classList.toggle('status-warning', warn);
        resultEl.classList.toggle('status-disconnected', !ok && !warn);
        resultEl.textContent = chipText;
    }
    showNotification(safe(detail), toastColor || (ok ? 'green' : 'red'), ok ? 4000 : 7000);
}

// Live per-row Test (issue #41). Under AutoPID this drives the ELM327; under the poll_log
// logger protocol the firmware runs the same test in-band on the poll task (identical JSON
// contract). Two poll_log-only outcomes get a yellow "can't test now" chip instead of a hard
// red error: a recording CSV trip blocks the test (code "trip_running"), and an engine-off /
// quiesced device can't transmit a PID request (code "engine_off").
async function runPidTest(entry) {
    const resultEl = entry.querySelector('.test-result');
    const buttonEl = entry.querySelector('.test-btn');
    if (!resultEl || !buttonEl) return;

    const payload = { kind: 'custom' };
    // #31 replaced the per-row free-text "Init" with a Mode dropdown and shows the PID box as
    // identifier-only, so compose the full wire string a Store would (rowPidWire) -- otherwise the
    // test would poll e.g. "0C" instead of "010C1". No init is sent (#57): the poll_log test
    // handler intentionally ignores init/pid_init (fixed 0x7E0 addressing, no ELM AT setup),
    // and the Custom Initialisation row is hidden -- poll_log is the only protocol this fork runs.
    payload.pid = rowPidWire(entry);
    payload.expr = rowStoredExpr(entry);   // friendly A/B/C -> raw Bn the firmware evaluates (issue #61)

    buttonEl.disabled = true;
    resultEl.style.display = 'inline-flex';
    resultEl.classList.add('status-indicator');
    resultEl.classList.remove('status-connected', 'status-disconnected', 'status-warning');
    resultEl.textContent = 'Testing…';

    try {
        const res = await fetch('/autopid/test_pid', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload),
        });
        const data = await res.json().catch(() => null);
        if (!res.ok || !data) {
            showTestOutcome(resultEl, false, 'Error', `Test request failed (HTTP ${res.status}).`);
            return;
        }
        if (data.ok) {
            let unit = (data.unit || '').trim();
            if (!unit) {
                unit = (entry.querySelector('.unit-input')?.value || '').trim();
            }
            const valueText = (data.value === null || data.value === undefined) ? '' : String(data.value);
            const shown = (unit ? `${valueText} ${unit}` : valueText).trim();
            showTestOutcome(resultEl, true, shown || 'OK', `Test OK — read ${shown || 'no value'}.`);
        } else if (data.code === 'trip_running') {
            showTestOutcome(resultEl, false, 'Trip running', data.error, 'yellow');
        } else if (data.code === 'engine_off') {
            showTestOutcome(resultEl, false, 'Engine off', data.error, 'yellow');
        } else {
            showTestOutcome(resultEl, false, 'Error', data.error ? `Test failed: ${data.error}` : 'Test failed.');
        }
    } catch (e) {
        showTestOutcome(resultEl, false, 'Error', `Test failed: ${e.message || e}`);
    } finally {
        buttonEl.disabled = false;
    }
}

async function runCanFilterTest(kind, entry) {
    const resultEl = entry.querySelector('.test-result');
    const buttonEl = entry.querySelector('.test-btn');
    if (!resultEl || !buttonEl) return;

    const frameIdStr = entry.querySelector('.frame-id-input')?.value || '';
    const expr = entry.querySelector('.expression-input')?.value || '';
    const unit = (entry.querySelector('.unit-input')?.value || '').trim();

    const frameIdNum = normalizeFrameIdInputToNumber(frameIdStr);
    if (frameIdNum === null) {
        showTestOutcome(resultEl, false, 'Bad ID', 'Invalid Frame ID — enter a hex value like 201 or 0x201.');
        return;
    }
    if (!expr.trim()) {
        showTestOutcome(resultEl, false, 'Empty', 'Missing expression.');
        return;
    }

    const payload = {
        kind,
        frame_id: frameIdNum,
        expr: expr,
    };

    buttonEl.disabled = true;
    resultEl.style.display = 'inline-flex';
    resultEl.classList.add('status-indicator');
    resultEl.classList.remove('status-connected', 'status-disconnected', 'status-warning');
    resultEl.textContent = 'Testing…';

    try {
        const res = await fetch('/autopid/test_can_filter', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload),
        });
        const data = await res.json().catch(() => null);
        if (!res.ok || !data) {
            showTestOutcome(resultEl, false, 'Error', `Test request failed (HTTP ${res.status}).`);
            return;
        }
        if (data.ok) {
            const valueText = (data.value === null || data.value === undefined) ? '' : String(data.value);
            const shown = (unit ? `${valueText} ${unit}` : valueText).trim();
            showTestOutcome(resultEl, true, shown || 'OK', `Test OK — read ${shown || 'no value'}.`);
        } else if (data.code === 'trip_running') {
            showTestOutcome(resultEl, false, 'Trip running', data.error, 'yellow');
        } else {
            showTestOutcome(resultEl, false, 'Error', data.error ? `Test failed: ${data.error}` : 'Test failed.');
        }
    } catch (e) {
        showTestOutcome(resultEl, false, 'Error', `Test failed: ${e.message || e}`);
    } finally {
        buttonEl.disabled = false;
    }
}

// A calculated channel is derived on-device from OTHER channels' values, so there
// is nothing to poll from the ECU to "test" — instead validate the expression the
// way the firmware parser reads it: check the syntax, then that every name it
// references is a channel configured on this page. Pure client-side, so it works
// regardless of the running protocol (the live PID/filter tests run under AutoPID or poll_log).
function runCalcTest(entry) {
    const resultEl = entry.querySelector('.test-result');
    const expr = (entry.querySelector('.expression-input')?.value || '').trim();
    if (!expr) {
        showTestOutcome(resultEl, false, 'Empty', 'Missing expression — enter something like "MAP - BARO".');
        return;
    }
    const parsed = parseCalcExpression(expr);
    if (!parsed.ok) {
        showTestOutcome(resultEl, false, 'Invalid', `Invalid expression: ${parsed.error}`);
        return;
    }
    const selfName = (entry.querySelector('.name-input')?.value || '').trim();
    const known = collectChannelNames(entry);
    const unknown = parsed.names.filter((n) => n !== selfName && !known.has(n));
    if (unknown.length) {
        // Syntax is fine; the names may be built-in channels the editor doesn't
        // list, so warn (yellow) rather than fail the test.
        showTestOutcome(resultEl, true, 'Check refs',
            `Expression parses, but no configured channel is named: ${unknown.join(', ')}. Ignore this if they are built-in channels.`,
            'yellow');
        return;
    }
    const note = parsed.names.length
        ? ` (references ${parsed.names.length} channel${parsed.names.length > 1 ? 's' : ''})`
        : ' (constant value)';
    showTestOutcome(resultEl, true, 'Valid', `Expression is valid${note}.`);
}

// Recursive-descent validator for calculated-channel expressions, mirroring the
// firmware grammar: numbers, channel identifiers, + - * /, parentheses and unary
// minus. No eval — every token is matched explicitly, so a malformed expression
// returns a clean error instead of throwing. Returns the referenced names on ok.
function parseCalcExpression(src) {
    const raw = src.match(/[A-Za-z_][A-Za-z0-9_]*|\d+(?:\.\d+)?|[-+*/()]|\S/g) || [];
    const tokens = [];
    for (const t of raw) {
        if (/^[A-Za-z_]/.test(t)) tokens.push({ type: 'name', text: t });
        else if (/^\d/.test(t)) tokens.push({ type: 'num', text: t });
        else if (t.length === 1 && '+-*/()'.includes(t)) tokens.push({ type: t, text: t });
        else return { ok: false, error: `unexpected "${t}"` };
    }
    if (!tokens.length) return { ok: false, error: 'empty expression' };

    let pos = 0;
    const names = new Set();
    const peek = () => tokens[pos];

    function parseExpr() {
        let r = parseTerm();
        if (!r.ok) return r;
        while (peek() && (peek().type === '+' || peek().type === '-')) {
            pos++;
            r = parseTerm();
            if (!r.ok) return r;
        }
        return { ok: true };
    }
    function parseTerm() {
        let r = parseFactor();
        if (!r.ok) return r;
        while (peek() && (peek().type === '*' || peek().type === '/')) {
            pos++;
            r = parseFactor();
            if (!r.ok) return r;
        }
        return { ok: true };
    }
    function parseFactor() {
        const t = peek();
        if (!t) return { ok: false, error: 'expression ends early' };
        if (t.type === '+' || t.type === '-') { pos++; return parseFactor(); }
        if (t.type === 'num') { pos++; return { ok: true }; }
        if (t.type === 'name') { names.add(t.text); pos++; return { ok: true }; }
        if (t.type === '(') {
            pos++;
            const inner = parseExpr();
            if (!inner.ok) return inner;
            if (!peek() || peek().type !== ')') return { ok: false, error: 'missing ")"' };
            pos++;
            return { ok: true };
        }
        return { ok: false, error: `unexpected "${t.text}"` };
    }

    const r = parseExpr();
    if (!r.ok) return r;
    if (pos !== tokens.length) return { ok: false, error: `unexpected "${tokens[pos].text}"` };
    return { ok: true, names: [...names] };
}

// The names a calculated expression may reference: the output name of every
// configured channel — polled PIDs, broadcast filters and other calculated
// channels — except the row being tested.
function collectChannelNames(selfEntry) {
    const names = new Set();
    document.querySelectorAll('.pid-entry, .custom-canfilter-entry, .calculated-entry').forEach((row) => {
        if (row === selfEntry) return;
        const n = (row.querySelector('.name-input')?.value || '').trim();
        if (n) names.add(n);
    });
    return names;
}

// Attribute-safe HTML escaper shared by the three row builders' innerHTML templates.
const safe = (v)=>String(v ?? '').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');

// The three "New ..." buttons land the fresh row at the TOP of its list, expanded,
// with the first field focused (issue #36). Only the button path promotes;
// loadAutoTable keeps appending, so a stored config loads back in array order.
function promoteNewEntry(entry) {
    if (!entry || !entry.parentNode) return;
    const container = entry.parentNode;
    if (container.firstElementChild !== entry) {
        container.insertBefore(entry, container.firstElementChild);
    }
    // .click() on the collapse control is a TOGGLE — expand only when the builder
    // emitted the row collapsed, so an already-expanded row is never re-collapsed.
    const content = entry.querySelector('.pid-content');
    if (content && getComputedStyle(content).display === 'none') {
        entry.querySelector('.collapse-btn')?.click();
    }
    content?.querySelector('input')?.focus();
}

// Delete drops the row (and, on the next Store, its CSV column) with no undo,
// so guard it behind a confirm that names the row by its title. Shared by all
// three list builders — each row carries a .pid-title.
function confirmDeleteRow(entry) {
    const label = entry.querySelector('.pid-title')?.textContent.trim();
    return confirm(label ? `Delete "${label}"?` : 'Delete this entry?');
}

// issue #33: moving the DOM row IS the reorder — storeAutoTableData walks the DOM
// in document order, so the saved array order (and the CSV column order that
// follows from it) tracks the rows with no serialization change. The handle
// drags via pointer events (its touch-action:none makes the same code work on
// phones), and ArrowUp/ArrowDown move one slot while it has focus, so reorder
// stays keyboard-accessible without dedicated ▲/▼ buttons.
function wireRowDrag(entry) {
    const handle = entry.querySelector('.drag-handle');
    if (!handle) return;

    // The whole .pid-header click toggles collapse; the handle must not.
    handle.addEventListener('click', (e) => e.stopPropagation());

    handle.addEventListener('keydown', (e) => {
        if (e.key !== 'ArrowUp' && e.key !== 'ArrowDown') return;
        e.preventDefault();
        e.stopPropagation();
        if (e.key === 'ArrowUp') {
            const prev = entry.previousElementSibling;
            if (!prev) return;
            entry.parentNode.insertBefore(entry, prev);
        } else {
            const next = entry.nextElementSibling;
            if (!next) return;
            entry.parentNode.insertBefore(entry, next.nextSibling);
        }
        handle.focus();   // reinsertion blurs the handle; keep keyboard flow
        enableAutoStoreButton();
    });

    handle.addEventListener('pointerdown', (e) => {
        if (e.button !== 0) return;
        e.preventDefault();
        e.stopPropagation();
        const container = entry.parentNode;
        const startNext = entry.nextElementSibling;
        const pointerId = e.pointerId;
        let lastY = e.clientY;
        let raf = 0;

        // Capture on document.body, NOT the handle: insertBefore disconnects the
        // row for an instant, and the browser silently releases pointer capture
        // on a disconnected element — captured on the handle, the drag froze
        // after the first swap. body is never disconnected, and the move/up
        // listeners sit on window so delivery never depends on what's under
        // the pointer.
        try { document.body.setPointerCapture(pointerId); } catch (_) {}
        entry.classList.add('dragging');

        // Reorder runs on a rAF loop rather than in pointermove: edge-autoscroll
        // must keep scrolling (and re-picking the drop slot) while the finger
        // holds still at the viewport edge, where no move events arrive.
        const tick = () => {
            const EDGE = 56;
            if (lastY < EDGE) {
                window.scrollBy(0, -Math.ceil((EDGE - lastY) / 4));
            } else if (lastY > window.innerHeight - EDGE) {
                window.scrollBy(0, Math.ceil((lastY - (window.innerHeight - EDGE)) / 4));
            }
            let before = null;
            for (const sib of container.children) {
                if (sib === entry) continue;
                const r = sib.getBoundingClientRect();
                if (lastY < r.top + r.height / 2) { before = sib; break; }
            }
            if (before !== entry.nextElementSibling) {
                container.insertBefore(entry, before);
            }
            raf = requestAnimationFrame(tick);
        };
        const onMove = (ev) => { if (ev.pointerId === pointerId) lastY = ev.clientY; };
        const finish = (ev) => {
            if (ev.pointerId !== pointerId) return;
            cancelAnimationFrame(raf);
            entry.classList.remove('dragging');
            window.removeEventListener('pointermove', onMove);
            window.removeEventListener('pointerup', finish);
            window.removeEventListener('pointercancel', finish);
            if (entry.nextElementSibling !== startNext) enableAutoStoreButton();
        };
        window.addEventListener('pointermove', onMove);
        window.addEventListener('pointerup', finish);
        window.addEventListener('pointercancel', finish);
        raf = requestAnimationFrame(tick);
    });
}

// --- Per-PID Mode / PID-text handling (issue #31): the single source in this file --
// "Mode" replaced the free-text per-PID "Init" (ELM ATSH strings the Datalogger
// protocol never executed), and the PID box shows only the IDENTIFIER (0C, 1746) --
// the service byte lives in the Mode dropdown and the ELM frames-hint nibble is
// hidden entirely (preserved per row, re-attached on save; it dies for real with
// issue #28). The STORED format is unchanged -- full "010C1"/"2217461" wire strings,
// what poll_log frames verbatim and pid_prefix_mode() in autopid_config.c derives
// the mode from (this block's C mirror) -- so a canonical row round-trips
// byte-identically: parsePidText() and composePidText() are exact inverses on it.
// A stored PID that does NOT parse canonically (exotic service, odd shape) keeps the
// pre-#31 UX: full string in the box, Mode display-only and derived from the prefix
// via pidServiceMode(), saved verbatim -- never blocked (it was storable before and
// firmware parses it fine). Old configs lose "Init" / gain "Mode" on their first
// Store (one-time migration; docs/internals/web_ui.md "Known exceptions").
// Mode 23 (ReadMemoryByAddress, issue #51) stores a 6-byte operand -- 4-byte big-endian
// address + 2-byte big-endian size -- so its "identifier" is 12 hex chars. The UI splits
// that into an Address box and a Size box; only the STORED string concatenates them, which
// is what keeps parsePidText/composePidText exact inverses here too.
const RMBA_MODE = '23';                        // the one mode whose operand is address+size
const RMBA_ADDR_LEN = 8;                       // hex chars of the address half of the identifier
const RMBA_SIZE_LEN = 4;                       // hex chars of the size half
const RMBA_MAX_SIZE = 6;                       // == POLLLOG_RMBA_MAX_SIZE: one ISO-TP single frame
const RMBA_ADDR_RE = new RegExp(`^[0-9A-Fa-f]{${RMBA_ADDR_LEN}}$`);
// ONE row per OBD service, so adding a mode is a single edit here plus the template <option>.
//   ident -- hex chars of the identifier the PID box holds
//   echo  -- operand bytes the ECU repeats back in its positive response, which is what
//            shifts the data window (see exprDataOffset). NOT the same as ident: mode 23
//            sends 6 operand bytes and echoes none.
// The three lookups below are DERIVED from it rather than maintained beside it -- the
// previous shape needed a comment instructing you to extend three tables together, which
// nothing enforced.
const PID_MODES = {
    '01': { ident: 2, echo: 1 },
    '22': { ident: 4, echo: 2 },
    '23': { ident: RMBA_ADDR_LEN + RMBA_SIZE_LEN, echo: 0 },
};
const EXPRESSIBLE_MODES = Object.keys(PID_MODES);                        // the dropdown's option set
const MODE_IDENT_LEN = Object.fromEntries(Object.entries(PID_MODES).map(([m, v]) => [m, v.ident]));
const MODE_ECHO_BYTES = Object.fromEntries(Object.entries(PID_MODES).map(([m, v]) => [m, v.echo]));
// Longest legal stored PID string: service(2) + the widest identifier + the frames hint(1),
// i.e. "23FFFFAC1800041" = 15. DERIVED, because the old hard-coded "< 10" silently rejected
// every mode 23 row the rest of this file can produce. It is also the real firmware ceiling:
// polllog_req_bytes reads at most POLLLOG_MAX_REQ_BYTES*2 nibbles plus the hint and TRUNCATES
// beyond that, so a longer string would be quietly polled as different bytes.
const PID_TEXT_MAX = 2 + Math.max(...Object.values(MODE_IDENT_LEN)) + 1;
const PID_HINT_DEFAULT = '1';                  // every shipped row uses "1 response frame"
const pidServiceMode = txt => {
    const p = String(txt || '').slice(0, 2);
    return /^[0-9A-Fa-f]{2}$/.test(p) ? p.toUpperCase() : '01';
};
// Stored wire string -> {service, ident, hint} when canonical, else null. ident/hint
// are kept VERBATIM (no case-fold) so compose reproduces the stored bytes exactly.
const parsePidText = txt => {
    const s = String(txt || '');
    if (!/^[0-9A-Fa-f]+$/.test(s)) return null;
    const service = s.slice(0, 2);
    if (!EXPRESSIBLE_MODES.includes(service)) return null;
    const len = MODE_IDENT_LEN[service];
    const rest = s.slice(2);
    let parsed = null;
    if (rest.length === len) parsed = { service, ident: rest, hint: '' };
    else if (rest.length === len + 1) parsed = { service, ident: rest.slice(0, len), hint: rest.slice(len) };
    if (!parsed) return null;
    // Mode 23 is the one service whose identifier carries a value the UI must be able to EDIT
    // (the read size) rather than merely display. A stored size outside 1..RMBA_MAX_SIZE has no
    // valid Size-box state, so treating it as canonical would load a row that then makes every
    // subsequent Store of the WHOLE page throw -- the user could not save an unrelated edit
    // without first deleting it. Falling through to null routes it down the existing legacy
    // path instead: full wire string in the box, Mode display-only, saved verbatim, never
    // blocked -- the same contract exotic PID shapes have had since #31. The firmware rejects
    // it independently (polllog_req_bytes) and now reports it as pids_unpollable.
    if (service === RMBA_MODE) {
        const size = rmbaSplitIdent(parsed.ident).size;
        if (!Number.isFinite(size) || size < 1 || size > RMBA_MAX_SIZE) return null;
    }
    return parsed;
};
const composePidText = (service, ident, hint) => service + ident + hint;
// Mode 23's identifier is two ideas glued together -- a 4-byte address then a 2-byte size --
// so the UI edits them in two boxes and only the STORED string concatenates them. Split/join
// are exact inverses: every legal size (1..6) is a single hex digit, so the zero-padded join
// reproduces the stored characters verbatim and the row still round-trips byte-identically.
const rmbaSplitIdent = ident => ({
    addr: String(ident).slice(0, RMBA_ADDR_LEN),
    size: parseInt(String(ident).slice(RMBA_ADDR_LEN), 16),
});
const rmbaJoinIdent = (addr, size) => String(addr) + Number(size).toString(16).padStart(RMBA_SIZE_LEN, '0');
// The one definition of "is this a usable mode 23 operand". Returns null when it is, else the
// reason. Shared by the save path and the sensor-file importer so the RULE lives once even
// though each caller words its own message (the importer names the file row, Store names the
// box). Mirrors the firmware floor in polllog_req_bytes.
function rmbaIdentProblem(addr, size) {
    if (!RMBA_ADDR_RE.test(addr)) {
        return `address must be 4 bytes (${RMBA_ADDR_LEN} hex chars) — e.g. FFFFAC18`;
    }
    if (!Number.isFinite(size) || size < 1 || size > RMBA_MAX_SIZE) {
        return `read size must be 1 to ${RMBA_MAX_SIZE} bytes — the ECU answers a Mode 23 read in a single frame`;
    }
    return null;
}
// Every byte a Mode 23 expression reads must lie inside the window that row actually REQUESTS:
// data occupies B{off}..B{off+size-1}. Modes 01/22 cannot be checked this way -- the response
// length is the ECU's business and unknowable from config -- but a Mode 23 row declares its own
// size, so a formula reaching past it is a config error we can name. Unchecked it is silent bad
// data: "FA" on a 2-byte read assembles a float from two real bytes and two ISO-TP padding
// bytes, logging a plausible wrong number forever with no error anywhere. Takes the STORED (Bn)
// form, so it sees exactly the indices the firmware will evaluate.
function rmbaExprProblem(storedExpr, size) {
    const off = exprDataOffset(RMBA_MODE);
    if (off == null || !Number.isFinite(size)) return null;
    const last = off + size - 1;
    let bad = null;
    String(storedExpr).replace(/([BSF])(\d+)/g, (tok, kind, digits) => {
        const n = parseInt(digits, 10);
        const end = kind === 'F' ? n + 3 : n;     // a float spans four bytes from n
        if (!bad && (n < off || end > last)) {
            const window = size === 1 ? 'data byte A' : `data bytes A–${String.fromCharCode(64 + size)}`;
            bad = `expression reads ${tok}, outside the ${size}-byte window this row requests (${window})`;
        }
        return tok;
    });
    return bad;
}
// The stored IDENTIFIER a canonical row would save: the PID box as typed, except Mode 23 where
// it is the address box joined with the size box. One definition, used by BOTH the live Test
// and the save path, so they can never disagree about the bytes that go on the wire.
function rowIdentText(entry, mode) {
    const box = entry.querySelector('.pid-input')?.value || '';
    if (mode !== RMBA_MODE) return box;
    const n = parseInt(entry.querySelector('.rmba-size-input')?.value, 10);
    return Number.isFinite(n) ? rmbaJoinIdent(box, n) : box;
}
// The wire-format PID string a row would STORE, so the live Test (issue #41) polls the exact
// bytes poll_log will: canonical rows compose service+ident+hint (mirrors storeAutoTableData's
// compose), legacy rows hold the full string in the box verbatim.
function rowPidWire(entry) {
    if (entry.dataset.pidCanonical === '1') {
        const mode = entry.querySelector('.mode-select')?.value || '01';
        const hint = entry.dataset.pidHint !== undefined ? entry.dataset.pidHint : PID_HINT_DEFAULT;
        return composePidText(mode, rowIdentText(entry, mode), hint);
    }
    return entry.querySelector('.pid-input')?.value || '';
}

// --- Standard-formula authoring for polled expressions (issue #61) -----------------
// Polled expressions are AUTHORED in the standard OBD vocabulary -- data bytes A, B, C,
// D... counted from the first DATA byte, exactly how SAE J1979 / Torque / OBD-Fusion PID
// tables print a formula like ((A*256)+B)/4 -- and STORED as the firmware's raw Bn
// indices, which include the ISO-TP framing (PCI + service/ident echo) that sits in front
// of the data. The two are related by the Mode's data offset off = 2 + ECHOED bytes --
// what the ECU repeats back, NOT what we send. For Modes 01/22 those coincide (1 and 2
// operand bytes echoed -> B3/B4), which is why this was once derived from MODE_IDENT_LEN;
// Mode 23 breaks that, because it sends a 6-byte operand and echoes NONE of it (the
// response is just 63 <data...>, bench-verified), putting its first data byte at B2. Hence
// an explicit echo table rather than a derivation: A<->B{off}, B<->B{off+1}, ...
// The swap is a per-token EXACT inverse on
// the standard subset, so a load->save of an unedited row reproduces the stored bytes
// verbatim (the byte-identical round-trip the rest of the editor guards). Only A-H are
// data-byte letters and 'B' is one ONLY when not followed by a digit, so raw Bn / Sn / V /
// numbers pass through untouched (\b keeps `B3` a byte token, never a bare data-byte B).
// An expression that reaches a framing byte, an OUT-OF-FRAME byte (B8+; poll_log evaluates a
// single 8-byte frame), or uses signed Sn has no clean letter form and stays in RAW Bn --
// the same canonical-vs-legacy split #31 uses for exotic PIDs, and gated on pidCanonical so
// the offset (hence the whole translation) is trusted only where the Mode is known. Nothing
// here changes the stored schema or what the firmware evaluates.
function exprDataOffset(mode) {
    const echo = MODE_ECHO_BYTES[mode];                        // PCI + service byte + echoed operand
    return echo === undefined ? null : 2 + echo;               // 01->3, 22->4, 23->2; unknown->null
}
function abcToBn(expr, off) {                                   // friendly A/B/C -> stored Bn
    if (off == null) return expr;                              // exprDataOffset yields null | number
    // Float token first (issue #51): "FA" means the float32 STARTING at data byte A, so the
    // letter belongs to the F, not to a bare data byte. The bare-letter pass below cannot
    // steal it anyway -- \b needs a non-word char before the letter and F is one short of
    // that -- but doing F first keeps the two rules independent of that subtlety.
    return expr.replace(/F([A-H])\b/g, (_, L) => 'F' + (off + L.charCodeAt(0) - 65))
               .replace(/\b([A-H])\b/g, (_, L) => 'B' + (off + L.charCodeAt(0) - 65));
}
function bnToAbc(expr, off) {                                   // stored Bn -> friendly A/B/C
    if (off == null) return expr;                              // exprDataOffset yields null | number
    // Fn spans FOUR bytes, so it earns a letter only when all of n..n+3 sit in the frame --
    // the same in-window rule as Bn, applied to the token's full width (issue #51).
    return expr.replace(/F(\d+)/g, (tok, d) => {
        const k = parseInt(d, 10);
        return (k >= off && k + 3 <= 7) ? 'F' + String.fromCharCode(65 + k - off) : tok;
    }).replace(/B(\d+)/g, (tok, d) => {
        const k = parseInt(d, 10);
        // Only bytes that physically exist in the 8-byte response frame (B0..B7) get a
        // letter. poll_log evaluates a single frame (poll_log.c: "always an 8-byte OBD
        // frame", no multi-frame reassembly), so B8+ is out of frame -- leave it raw rather
        // than prettify it into a name that would mask the out-of-bounds read.
        return (k >= off && k <= 7) ? String.fromCharCode(65 + k - off) : tok;
    });
}
// The friendly A/B/C form of a stored Bn expression, or null when it isn't A/B/C-representable
// -- i.e. every byte ref sits in the data window (bnToAbc leaves no raw B/S token behind) AND
// the forward map reproduces it exactly. That round-trip check is what guarantees a friendly
// row saves byte-identically to its origin. Returning the string (not a bool) lets the caller
// reuse it as the display value instead of translating a second time.
// "Still names a numbered byte" -- an out-of-window Bn, a signed Sn, or an out-of-window
// float Fn. One definition, used both here and by the sensor-file importer to tell a wire
// expression from a friendly one.
const exprHasWireByte = e => /[BSF]\d/.test(e);
function exprAsAbc(bnExpr, off) {
    if (off == null) return null;
    const abc = bnToAbc(bnExpr, off);
    if (exprHasWireByte(abc)) return null;                     // out-of-window byte or signed -> raw
    return abcToBn(abc, off) === bnExpr ? abc : null;
}
// One-time normalization to the market arithmetic form (issue #61): rewrite the firmware's
// compact unsigned big-endian range [Bx:By] into ((Bx*256^k)+...+By) -- the notation Torque /
// OBD-Fusion / SAE PID tables use -- so the friendly view reads ((A*256)+B)/4 rather than
// [A:B]/4. Applied only where a row is shown friendly, so it touches exactly the A/B/C rows.
// Signed [Sx:Sy] (different sign-extending assembly) and ranges wider than 4 bytes (uint32 is
// the exact-in-double range) are left compact. The VALUE is unchanged: evaluate_expression
// computes [Bx:By] and the polynomial identically for byte data. Idempotent (no [..] left to
// convert on a second pass), so a migrated config then round-trips byte-identically.
function rangeToArith(expr) {
    return String(expr).replace(/\[B(\d+):B(\d+)\]/g, (m, aStr, bStr) => {
        const a = parseInt(aStr, 10), b = parseInt(bStr, 10);
        const n = b - a + 1;
        if (b < a || n > 4) return m;                  // invalid or too wide -> keep compact
        if (n === 1) return 'B' + a;                   // a 1-byte range is just the byte
        const terms = [];
        for (let j = a; j <= b; j++) {                 // big-endian: highest byte gets the largest weight
            const shift = b - j;
            terms.push(shift === 0 ? ('B' + j) : ('(B' + j + '*' + Math.pow(256, shift) + ')'));
        }
        return '(' + terms.join('+') + ')';
    });
}
// The stored/firmware Bn expression for a polled row: friendly rows (exprAbc='1') translate
// A/B/C back to Bn against the row's CURRENT Mode offset (so flipping Mode 01<->22 re-frames
// B3<->B4 on its own) and normalize any typed range to arithmetic; raw rows -- and every
// non-canonical PID -- store the box verbatim.
function rowStoredExpr(entry) {
    const raw = entry.querySelector('.expression-input')?.value || '';
    if (entry.dataset.exprAbc !== '1') return raw;
    return rangeToArith(abcToBn(raw, exprDataOffset(entry.querySelector('.mode-select')?.value || '01')));
}
// Custom-filter rows read RAW broadcast frames: there is no ISO-TP framing to strip and no
// SAE A/B/C convention for arbitrary CAN payloads, so bytes are referenced directly as Bn
// (that IS the mainstream notation for raw CAN). A fixed clarifying label, no translation.
const EXPR_HINT_BROADCAST = 'Raw broadcast frame - no OBD framing to strip, so the CAN '
    + 'payload starts at B0. Reference bytes directly (B0 B1 B2 ...); e.g. a 16-bit '
    + 'big-endian value is [B0:B1]. The A/B/C data-byte names apply to polled PIDs only.';

function addCollapsibleRow(rowData = {}) {
    const container = document.querySelector('.pid-entries');
    const entry = document.createElement('div');
    entry.className = 'pid-entry';

    const enabledChecked = (rowData.enabled === false || rowData.Enabled === false) ? '' : 'checked';

    entry.innerHTML = `
        <div class="pid-header">
            <div class="header-left">
                <button type="button" class="collapse-btn">▸</button>
                <span class="pid-title">New PID</span>
            </div>
            <div class="header-right">
                <button type="button" class="drag-handle" title="Drag to reorder (Arrow keys move the row)" aria-label="Reorder">⋮⋮</button>
                <span class="test-result status-indicator" style="display:none"></span>
                <button type="button" class="test-btn">Test</button>
                <label class="enabled-label" style="display:flex; align-items:center; gap:4px; font-size:0.7rem;">
                    <input type="checkbox" class="enabled-chk" ${enabledChecked}>
                    Enabled
                </label>
                <button type="button" class="delete-btn">Delete</button>
            </div>
        </div>
        <div class="pid-content hidden">
            <table class="compact-form-table">
                <tr>
                    <td>Name:&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;</td>
                    <td><input type="text" class="name-input" value="${safe(rowData.Name || '')}"
                        placeholder="Parameter Name"></td>
                </tr>
                <!-- Mode (issue #31): declarative OBD service selector replacing the free-text
                     per-PID "Init"; hydrated/synced from the PID text -- full story at
                     pidServiceMode() above addCollapsibleRow. The <option> values MUST stay
                     in lockstep with EXPRESSIBLE_MODES. No id= / inline on*= -- same
                     lint_web.py rationale as Sample Rate below. -->
                <tr>
                    <td>Mode:</td>
                    <td>
                        <select class="mode-select">
                            <option value="01">01 &mdash; standard OBD</option>
                            <option value="22">22 &mdash; extended (DID)</option>
                            <option value="23">23 &mdash; memory (address)</option>
                        </select>
                    </td>
                </tr>
                <tr>
                    <td class="pid-label">PID:</td>
                    <td><input type="text" class="pid-input" value="${safe(rowData.PID || '')}"
                        placeholder="PID"></td>
                </tr>
                <!-- Mode 23 read size (issue #51): bytes to read at the address. The ECU answers
                     63 <data...>, and this path receives ONE ISO-TP single frame, so 1..6
                     (RMBA_MAX_SIZE, == POLLLOG_RMBA_MAX_SIZE in poll_log.c). A number input rather
                     than a <select>: a select must enumerate the legal sizes, and a hand-written
                     config carrying a size the list happened to lack would silently save back a
                     DIFFERENT size. Row shown only while Mode is 23; no id= / inline on*= -- same
                     lint_web.py rationale as Sample Rate below. -->
                <tr class="rmba-size-row" style="display:none">
                    <td>Read size:</td>
                    <td><input type="number" class="rmba-size-input" min="1" max="${RMBA_MAX_SIZE}" step="1"
                               value="" placeholder="bytes" style="width:5em">
                        <span class="rmba-size-hint"></span></td>
                </tr>
                <tr>
                    <td>Expression:</td>
                    <td><input type="text" class="expression-input" value="${safe(rowData.Expression || '')}"
                        placeholder="Standard formula, e.g. ((A*256)+B)/4  (A B C D = data bytes; FA = float32 at A)"></td>
                </tr>
                <tr>
                    <td>Unit:</td>
                    <td><input type="text" class="unit-input" value="${safe(rowData.Unit || '')}"
                        placeholder="e.g. V, °C, kPa"></td>
                </tr>
                <!-- Sample Rate (issue #29): the per-PID sweep divisor "SampleEvery". No id= and
                     no inline on*= handler -- both would be invisible to lint_web.py (check 1
                     scans getElementById literals, check 2 scans homepage_full.html only), so the
                     control is wired with addEventListener below, like .delete-btn/.collapse-btn.
                     The custom field's max MUST stay equal to AUTOPID_MAX_SAMPLE_EVERY
                     (components/autopid/autopid.h). -->
                <tr>
                    <td>Sample Rate:</td>
                    <td>
                        <select class="sample-every-select">
                            <option value="0">Every sweep</option>
                            <option value="2">Every 2nd sweep</option>
                            <option value="4">Every 4th sweep</option>
                            <option value="8">Every 8th sweep</option>
                            <option value="16">Every 16th sweep</option>
                            <option value="custom">Custom&hellip;</option>
                        </select>
                        <input type="number" class="sample-every-custom" min="2" max="64" step="1"
                               value="" placeholder="N" style="width:5em;display:none">
                        <span class="sample-every-hint"></span>
                    </td>
                </tr>
                <!-- Class + Period hidden: Class is upstream HA/MQTT sensor metadata nothing
                     in this firmware consumes, and Period is honored only by the Legacy
                     AutoPID scheduler -- the Datalogger (poll_log) protocol polls every PID
                     each sweep regardless. Inputs stay in the DOM so storeAutoTableData()
                     keeps round-tripping the auto_pid.json keys. -->
                <tr style="display:none">
                    <td>Class:</td>
                    <td><input type="text" class="class-input" value="${safe(rowData.Class || '')}"
                        placeholder="e.g. voltage, temp"></td>
                </tr>
                <tr>
                    <td>Min Value:</td>
                    <td><input type="number" class="min-value-input" value="${safe(rowData.MinValue || '')}"
                        step="0.01" placeholder="Minimum value"></td>
                </tr>
                <tr>
                    <td>Max Value:</td>
                    <td><input type="number" class="max-value-input" value="${safe(rowData.MaxValue || '')}"
                        step="0.01" placeholder="Maximum value"></td>
                </tr>
                <tr>
                    <td>Description:</td>
                    <td><input type="text" class="description-input" value="${safe(rowData.description || '')}"
                        placeholder="What this PID measures"></td>
                </tr>
                <tr>
                    <td>Comment:</td>
                    <td><input type="text" class="comment-input" value="${safe(rowData.comment || '')}"
                        placeholder="Optional note (never sent to the ECU)"></td>
                </tr>
                <tr style="display:none">
                    <td>Period(ms):</td>
                    <td><input type="number" class="period-input" value="${safe(rowData.Period || '')}"
                        placeholder="ms"></td>
                </tr>
            </table>
        </div>
    `;
console.log("addCollapsibleRow:", rowData);
const style = document.createElement('style');
style.textContent = pidEntryStyles;
document.head.appendChild(style);
const header = entry.querySelector('.pid-header');
const deleteBtn = entry.querySelector('.delete-btn');
const collapseBtn = entry.querySelector('.collapse-btn');
const content = entry.querySelector('.pid-content');
const parameterTitle = entry.querySelector('.pid-title');
const nameInput = entry.querySelector('.name-input');
const pidInput = entry.querySelector('.pid-input');
const enabledChk = entry.querySelector('.enabled-chk');

if (enabledChk) {
    enabledChk.addEventListener('click', (e) => e.stopPropagation());
    enabledChk.addEventListener('change', enableAutoStoreButton);
}

deleteBtn.addEventListener('click', () => {
    if (!confirmDeleteRow(entry)) return;
    entry.remove();
    enableAutoStoreButton();
});

const testBtn = entry.querySelector('.test-btn');
if (testBtn) {
    testBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        runPidTest(entry);
    });
}

const toggleCollapse = (e) => {
    e.stopPropagation();
    const isHidden = content.style.display === 'none' || getComputedStyle(content).display === 'none';
    content.style.display = isHidden ? 'block' : 'none';
    collapseBtn.textContent = isHidden ? '▾' : '▸';
};

header.addEventListener('click', toggleCollapse);
collapseBtn.addEventListener('click', toggleCollapse);

parameterTitle.textContent = rowData.Name ? rowData.Name : 'New PID';
const updateTitle = () => {
    parameterTitle.textContent = `${nameInput.value || 'New Parameter'}`;
};

nameInput.addEventListener('input', updateTitle);
pidInput.addEventListener('input', updateTitle);

// --- Mode + PID box (issue #31; full story at parsePidText) -----------------------
// Canonical rows: box = identifier only (0C, 1746), select = the service and the
// SOURCE of the stored prefix on save, frames-hint stashed invisibly on the row.
// Legacy/exotic rows (stored string doesn't parse canonically): box keeps the
// verbatim string, select is display-only (derived, one-way synced from typing) and
// save stores the box verbatim. A NEW row is canonical from birth: Mode 01, empty
// box, default hint.
const modeSel = entry.querySelector('.mode-select');
const pidParsed = rowData.PID ? parsePidText(rowData.PID)
                              : { service: '01', ident: '', hint: PID_HINT_DEFAULT };
entry.dataset.pidCanonical = pidParsed ? '1' : '0';
entry.dataset.pidHint = pidParsed ? pidParsed.hint : '';
if (pidParsed) {
    // Mode 23 (issue #51) stores address+size glued together, so the box gets only the address
    // half and the Size box below gets the rest. Gated on canonical for the same reason the
    // A/B/C translation is: only there is the shape trusted. The placeholder is owned by
    // syncRmbaRow() (called at the end of this block), which knows both cases.
    pidInput.value = pidParsed.service === RMBA_MODE
        ? rmbaSplitIdent(pidParsed.ident).addr
        : pidParsed.ident;              // programmatic set: fires no event, dirties nothing
    modeSel.value = pidParsed.service;
} else {
    modeSel.value = pidServiceMode(pidInput.value) === '22' ? '22' : '01';
    pidInput.addEventListener('input', () => {
        const m = pidServiceMode(pidInput.value);
        modeSel.value = EXPRESSIBLE_MODES.includes(m) ? m : '01';
    });
}
// Mode 23 (issue #51): show the Size box and relabel PID -> Address whenever this row is a
// canonical memory read. A LEGACY row keeps the whole verbatim string in the box and gets no
// Size row, exactly as modes 01/22 do.
const rmbaSizeRow   = entry.querySelector('.rmba-size-row');
const rmbaSizeInput = entry.querySelector('.rmba-size-input');
const rmbaSizeHint  = entry.querySelector('.rmba-size-hint');
const rmbaPidLabel  = entry.querySelector('.pid-label');
const syncRmbaRow = () => {
    const on = entry.dataset.pidCanonical === '1' && modeSel.value === RMBA_MODE;
    if (rmbaSizeRow) rmbaSizeRow.style.display = on ? '' : 'none';
    if (rmbaPidLabel) rmbaPidLabel.textContent = on ? 'Address:' : 'PID:';
    if (entry.dataset.pidCanonical === '1') {
        pidInput.placeholder = on ? 'e.g. FFFFAC18' : 'e.g. 0C or 1746';
    }
    // Name the data-byte letters this size actually yields, so the Expression box below is
    // authored against a window the user can see (A is B2 for mode 23, not B3/B4).
    if (rmbaSizeHint) {
        const n = parseInt(rmbaSizeInput && rmbaSizeInput.value, 10);
        rmbaSizeHint.textContent = (on && Number.isFinite(n) && n >= 1 && n <= RMBA_MAX_SIZE)
            ? (n === 1 ? '  → data byte A' : '  → data bytes A–' + String.fromCharCode(64 + n))
            : '';
    }
};
if (pidParsed && pidParsed.service === RMBA_MODE && rmbaSizeInput) {
    const size = rmbaSplitIdent(pidParsed.ident).size;   // address half already went in the box above
    rmbaSizeInput.value = Number.isFinite(size) ? size : '';
}
modeSel.addEventListener('change', syncRmbaRow);
if (rmbaSizeInput) rmbaSizeInput.addEventListener('input', syncRmbaRow);
syncRmbaRow();
// Standard-formula authoring (issue #61): show the stored Bn expression in the OBD A/B/C
// vocabulary and translate it back on save. Only canonical PID rows qualify -- their Mode
// (hence the data offset) is known and trustworthy; legacy/exotic PID shapes keep raw Bn.
// A stored expression that dips into a framing byte or uses signed Sn also stays raw
// (exprAsAbc null). Programmatic .value set fires no event -> dirties nothing.
const exprInput = entry.querySelector('.expression-input');
const exprOff0 = exprDataOffset(modeSel.value);
const exprCanonical = entry.dataset.pidCanonical === '1' && exprInput;
// Normalize the compact [Bx:By] range to the market arithmetic form BEFORE classifying, so a
// bracket-form config migrates to ((Bx*256)+By) and displays like Torque/OBD-Fusion. Only a
// canonical PID row is a migration candidate; a non-canonical row keeps its box verbatim.
const exprStored0 = exprCanonical ? rangeToArith(exprInput.value) : '';
// exprAsAbc returns the friendly string ('' for an empty box -- a NEW row is canonical from
// birth, so it MUST stay exprAbc='1' or a later-typed A/B/C formula would be stored verbatim
// and rejected by the firmware parser) or null when the stored expr can't be shown friendly.
const exprFriendly0 = exprCanonical ? exprAsAbc(exprStored0, exprOff0) : null;
if (exprFriendly0 !== null) {
    exprInput.value = exprFriendly0;   // display the market A/B/C form; save re-derives Bn
    entry.dataset.exprAbc = '1';
} else {
    entry.dataset.exprAbc = '0';
}
// No live preview: the friendly A/B/C stays in the box; rowStoredExpr() translates it back to
// raw Bn (re-framing B3<->B4 against the row's CURRENT Mode) only at save/test time.

// --- Sample Rate (per-PID sweep divisor, issue #29) -------------------------------
const SAMPLE_PRESETS = ['0', '2', '4', '8', '16'];
const sampleSel    = entry.querySelector('.sample-every-select');
const sampleCustom = entry.querySelector('.sample-every-custom');

// Hydrate. 0, 1, absent and any garbage all mean "every sweep": the poll_log gate treats
// N<2 as a no-op, so the UI canonicalizes the same way or a save would invent a distinction
// the firmware does not have. parseInt(undefined,10) is NaN -> handled -> a brand-new row
// from addRowAutoTable() correctly defaults to "Every sweep".
let sampleN = parseInt(rowData.SampleEvery, 10);
if (!Number.isFinite(sampleN) || sampleN < 2) sampleN = 0;
if (sampleN > 64) sampleN = 64;      // must equal AUTOPID_MAX_SAMPLE_EVERY (autopid.h)
if (SAMPLE_PRESETS.includes(String(sampleN))) {
    sampleSel.value = String(sampleN);
    sampleCustom.value = '';
    sampleCustom.style.display = 'none';
} else {
    sampleSel.value = 'custom';               // e.g. a hand-written N=5
    sampleCustom.value = String(sampleN);
    sampleCustom.style.display = '';
}
sampleSel.addEventListener('change', () => {
    const custom = sampleSel.value === 'custom';
    sampleCustom.style.display = custom ? '' : 'none';
    if (custom && !sampleCustom.value) { sampleCustom.value = '3'; sampleCustom.focus(); }
    updateAllSampleHints();
});
sampleCustom.addEventListener('input', updateAllSampleHints);
if (enabledChk) enabledChk.addEventListener('change', updateAllSampleHints);

entry.querySelectorAll('input, select').forEach(input => {
    input.addEventListener('input', enableAutoStoreButton);
});

wireRowDrag(entry);

container.appendChild(entry);
// After the append, not before: updateAllSampleHints() walks document .pid-entry rows, so
// running it while `entry` is still detached would leave THIS row's hint blank until the
// next edit. Sets textContent only and dispatches no event -> does not dirty Store.
updateAllSampleHints();
return entry;
}

function normalizeFrameIdInputToNumber(v) {
    if (v === null || v === undefined) return null;
    const s = String(v).trim();
    if (!s) return null;

    let n;
    if (/^0x[0-9a-f]+$/i.test(s)) {
        n = parseInt(s, 16);
    } else if (/^[0-9]+$/.test(s)) {
        n = parseInt(s, 10);
    } else if (/^[0-9a-f]+$/i.test(s)) {
        // Allow hex without 0x
        n = parseInt(s, 16);
    } else {
        return null;
    }
    if (!Number.isFinite(n) || n < 0) return null;
    return n;
}

function formatFrameIdForUi(n) {
    if (typeof n !== 'number' || !Number.isFinite(n)) return '';
    return '0x' + n.toString(16).toUpperCase();
}

function addCustomCanFilterEntry(rowData = {}) {
    const container = document.querySelector('.custom-canfilter-entries');
    if (!container) return;

    const frameIdValue = (rowData.frame_id !== undefined && rowData.frame_id !== null)
        ? (typeof rowData.frame_id === 'number' ? formatFrameIdForUi(rowData.frame_id) : String(rowData.frame_id))
        : '';
    const p = rowData.parameter || (Array.isArray(rowData.parameters) ? rowData.parameters[0] : {}) || {};

    const entry = document.createElement('div');
    entry.className = 'custom-canfilter-entry';

    const titleText = `${frameIdValue || 'Frame'} - ${(p.name || rowData.name || 'New Parameter')}`;

    entry.innerHTML = `
        <div class="pid-header">
            <div class="header-left">
                <button type="button" class="collapse-btn">▸</button>
                <span class="pid-title">${safe(titleText)}</span>
            </div>
            <div class="header-right">
                <button type="button" class="drag-handle" title="Drag to reorder (Arrow keys move the row)" aria-label="Reorder">⋮⋮</button>
                <span class="test-result status-indicator" style="display:none"></span>
                <button type="button" class="test-btn">Test</button>
                <label class="enabled-label" style="display:flex; align-items:center; gap:4px; font-size:0.7rem;">
                    <input type="checkbox" class="enabled-chk" ${(p.enabled === false || rowData.enabled === false) ? '' : 'checked'}>
                    Enabled
                </label>
                <button type="button" class="delete-btn">Delete</button>
            </div>
        </div>
        <div class="pid-content" style="display: none;">
            <table class="compact-form-table">
                <tr>
                    <td>Frame ID:</td>
                    <td><input type="text" class="frame-id-input" value="${safe(frameIdValue)}" placeholder="0x7E8 or 2024"></td>
                </tr>
                <tr>
                    <td>Name:</td>
                    <td><input type="text" class="name-input" value="${safe(p.name || rowData.name || 'New Parameter')}" placeholder="Parameter Name"></td>
                </tr>
                <tr>
                    <td>Expression:</td>
                    <td><input type="text" class="expression-input" value="${safe(p.expression)}" placeholder="Expression"><div class="expr-hint"></div></td>
                </tr>
                <tr>
                    <td>Unit:</td>
                    <td><input type="text" class="unit-input" value="${safe(p.unit)}" placeholder="Unit"></td>
                </tr>
                <!-- Class + Period hidden here too: only the Legacy AutoPID ATMA monitor
                     (process_can_filter_frame) honors a filter parameter's period; poll_log
                     and fast_log decode broadcasts with their own fixed throttles. Inputs
                     stay in the DOM for the auto_pid.json round-trip. -->
                <tr style="display:none">
                    <td>Class:</td>
                    <td><input type="text" class="class-input" value="${safe(p.class)}" placeholder="Class"></td>
                </tr>
                <tr>
                    <td>Min Value:</td>
                    <td><input type="number" class="min-input" value="${safe(p.min)}" step="0.01" placeholder="Min"></td>
                </tr>
                <tr>
                    <td>Max Value:</td>
                    <td><input type="number" class="max-input" value="${safe(p.max)}" step="0.01" placeholder="Max"></td>
                </tr>
                <tr>
                    <td>Description:</td>
                    <td><input type="text" class="description-input" value="${safe(p.description)}" placeholder="What this parameter measures"></td>
                </tr>
                <tr>
                    <td>Comment:</td>
                    <td><input type="text" class="comment-input" value="${safe(p.comment)}" placeholder="Optional note (never sent to the ECU)"></td>
                </tr>
                <tr style="display:none">
                    <td>Period(ms):</td>
                    <td><input type="number" class="period-input" value="${safe(p.period || '5000')}" min="100" max="60000"></td>
                </tr>
            </table>
        </div>
    `;

    const style = document.createElement('style');
    style.textContent = pidEntryStyles;
    document.head.appendChild(style);

    const header = entry.querySelector('.pid-header');
    const deleteBtn = entry.querySelector('.delete-btn');
    const collapseBtn = entry.querySelector('.collapse-btn');
    const content = entry.querySelector('.pid-content');
    const titleEl = entry.querySelector('.pid-title');
    const enabledChk = entry.querySelector('.enabled-chk');

    // Byte-offset legend (issue #61): custom filters evaluate RAW broadcast frames (no
    // ISO-TP framing), so data always starts at B0 -- static, no Mode to react to.
    const exprHint = entry.querySelector('.expr-hint');
    if (exprHint) exprHint.textContent = EXPR_HINT_BROADCAST;

    if (enabledChk) {
        enabledChk.addEventListener('click', (e) => e.stopPropagation());
        enabledChk.addEventListener('change', enableAutoStoreButton);
    }

    deleteBtn.addEventListener('click', () => {
        if (!confirmDeleteRow(entry)) return;
        entry.remove();
        enableAutoStoreButton();
    });

    const testBtn = entry.querySelector('.test-btn');
    if (testBtn) {
        testBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            runCanFilterTest('custom', entry);
        });
    }

    const toggleCollapse = (e) => {
        e.stopPropagation();
        const isHidden = content.style.display === 'none';
        content.style.display = isHidden ? 'block' : 'none';
        collapseBtn.textContent = isHidden ? '▾' : '▸';
    };
    header.addEventListener('click', toggleCollapse);
    collapseBtn.addEventListener('click', toggleCollapse);

    const updateTitle = () => {
        const fid = entry.querySelector('.frame-id-input')?.value?.trim() || 'Frame';
        const nm = entry.querySelector('.name-input')?.value?.trim() || 'New Parameter';
        titleEl.textContent = `${fid} - ${nm}`;
    };

    entry.querySelectorAll('input, select').forEach(input => {
        input.addEventListener('input', () => { updateTitle(); enableAutoStoreButton(); });
        input.addEventListener('change', () => { updateTitle(); enableAutoStoreButton(); });
    });

    wireRowDrag(entry);

    container.appendChild(entry);
    enableAutoStoreButton();
    return entry;
}

function addCustomFilterRow() {
    promoteNewEntry(addCustomCanFilterEntry({
        frame_id: '',
        parameter: { name: 'New Parameter', expression: '', unit: '', class: '', period: '5000', min: '', max: '' }
    }));
}

// Calculated channels (Task #17): a derived channel computed on-device from OTHER channel
// values (source "CALC"). Expression references channel NAMES, e.g. "MAP - BARO" or
// "EQ_RATIO * 14.64" (operators + - * /, parens, unary minus). Mirrors the custom-filter row.
function addCalculatedChannelEntry(rowData = {}) {
    const container = document.querySelector('.calculated-entries');
    if (!container) return;

    const name = (rowData.name !== undefined && rowData.name !== null) ? String(rowData.name) : 'New Channel';
    const expr = (rowData.expression !== undefined && rowData.expression !== null) ? String(rowData.expression) : '';
    const unit = (rowData.unit !== undefined && rowData.unit !== null) ? String(rowData.unit) : '';
    const enabled = (rowData.enabled === false) ? false : true;

    const entry = document.createElement('div');
    entry.className = 'calculated-entry';

    entry.innerHTML = `
        <div class="pid-header">
            <div class="header-left">
                <button type="button" class="collapse-btn">▸</button>
                <span class="pid-title">${safe(name)}</span>
            </div>
            <div class="header-right">
                <button type="button" class="drag-handle" title="Drag to reorder (Arrow keys move the row)" aria-label="Reorder">⋮⋮</button>
                <span class="test-result status-indicator" style="display:none"></span>
                <button type="button" class="test-btn">Test</button>
                <label class="enabled-label" style="display:flex; align-items:center; gap:4px; font-size:0.7rem;">
                    <input type="checkbox" class="enabled-chk" ${enabled ? 'checked' : ''}>
                    Enabled
                </label>
                <button type="button" class="delete-btn">Delete</button>
            </div>
        </div>
        <div class="pid-content" style="display: none;">
            <table class="compact-form-table">
                <tr>
                    <td>Name:</td>
                    <td><input type="text" class="name-input" value="${safe(name)}" placeholder="Channel Name"></td>
                </tr>
                <tr>
                    <td>Expression:</td>
                    <td><input type="text" class="expression-input" value="${safe(expr)}" placeholder="e.g. MAP - BARO"></td>
                </tr>
                <tr>
                    <td>Unit:</td>
                    <td><input type="text" class="unit-input" value="${safe(unit)}" placeholder="Unit"></td>
                </tr>
            </table>
        </div>
    `;

    const style = document.createElement('style');
    style.textContent = pidEntryStyles;
    document.head.appendChild(style);

    const header = entry.querySelector('.pid-header');
    const deleteBtn = entry.querySelector('.delete-btn');
    const collapseBtn = entry.querySelector('.collapse-btn');
    const content = entry.querySelector('.pid-content');
    const titleEl = entry.querySelector('.pid-title');
    const enabledChk = entry.querySelector('.enabled-chk');

    if (enabledChk) {
        enabledChk.addEventListener('click', (e) => e.stopPropagation());
        enabledChk.addEventListener('change', enableAutoStoreButton);
    }

    deleteBtn.addEventListener('click', () => {
        if (!confirmDeleteRow(entry)) return;
        entry.remove();
        enableAutoStoreButton();
    });

    const testBtn = entry.querySelector('.test-btn');
    if (testBtn) {
        testBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            runCalcTest(entry);
        });
    }

    const toggleCollapse = (e) => {
        e.stopPropagation();
        const isHidden = content.style.display === 'none';
        content.style.display = isHidden ? 'block' : 'none';
        collapseBtn.textContent = isHidden ? '▾' : '▸';
    };
    header.addEventListener('click', toggleCollapse);
    collapseBtn.addEventListener('click', toggleCollapse);

    const updateTitle = () => {
        titleEl.textContent = entry.querySelector('.name-input')?.value?.trim() || 'New Channel';
    };

    entry.querySelectorAll('input').forEach(input => {
        input.addEventListener('input', () => { updateTitle(); enableAutoStoreButton(); });
        input.addEventListener('change', () => { updateTitle(); enableAutoStoreButton(); });
    });

    wireRowDrag(entry);

    container.appendChild(entry);
    enableAutoStoreButton();
    return entry;
}

function addCalculatedRow() {
    promoteNewEntry(addCalculatedChannelEntry({ name: 'New Channel', expression: '', unit: '', enabled: true }));
}

function loadAutoTable(jsonData) {
    try {
        console.log("Raw jsonData:", jsonData);
        const data = jsonData;

        // Reset polled PIDs UI to avoid duplicates: this loader REPLACES the tables, it
        // never appends. Inert at page load (the container starts empty), but loading into
        // a populated table is a real path for any caller that reloads the set (the sensor
        // import, issue #63) -- without this, polled rows would append while the two
        // containers below were replaced.
        const pidContainer = document.querySelector('.pid-entries');
        if (pidContainer) pidContainer.innerHTML = '';

        // Reset custom filters UI to avoid duplicates on reload
        const customFilterContainer = document.querySelector('.custom-canfilter-entries');
        if (customFilterContainer) customFilterContainer.innerHTML = '';

        // Reset calculated channels UI (Task #17) to avoid duplicates on reload
        const calculatedContainer = document.querySelector('.calculated-entries');
        if (calculatedContainer) calculatedContainer.innerHTML = '';

        const initialisationElement = document.getElementById("initialisation");
        if (initialisationElement) {
            initialisationElement.value = data.initialisation || '';
        }

        const automateTable = document.getElementById("automate_table");
        if (!automateTable) {
            console.error("PID table not found");
            return;
        }

        const setElementValue = (id, value, defaultValue = '') => {
            const element = document.getElementById(id);
            if (element) {
                element.value = value || defaultValue;
            }
        };

        setElementValue("disable_on_sleep_voltage", data.disable_on_sleep_voltage, 'disable');
        setElementValue("pid_polling_min_voltage", data.pid_polling_min_voltage, '13.1');
        const pidMinVoltEl = document.getElementById("pid_polling_min_voltage");
        const pidMinVoltValEl = document.getElementById("pid_polling_min_voltage_value");
        if (pidMinVoltEl && pidMinVoltValEl) pidMinVoltValEl.textContent = pidMinVoltEl.value;
        // Standard-PIDs keys have no UI after the streamline (issue #21); capture them so
        // storeAutoTableData re-emits the stored values verbatim instead of defaults.
        loadedStandardPids = data.standard_pids || 'disable';
        loadedEcuProtocol = data.ecu_protocol || '6';

        if (data.pids && Array.isArray(data.pids)) {
            data.pids.forEach((pidData, index) => {
                console.log(`Loading PID ${index}:`, pidData);
                // Legacy per-PID Init (pre-#31) is dropped on the next Store -- an intended
                // one-time migration. Every shipped value was "" or "ATSH7E0;", both already
                // covered by the firmware's hardcoded 7E0; surface anything else rather than
                // silently discarding it. A stored "Mode" key is deliberately NOT hydrated:
                // the row builder parses the PID wire string itself (parsePidText) and the
                // save path recomposes it, so the key cannot go stale here.
                if (pidData.Init && pidData.Init !== 'ATSH7E0;') {
                    console.warn(`PID "${pidData.Name}": dropping legacy Init "${pidData.Init}" (replaced by Mode, issue #31)`);
                }
                addCollapsibleRow({
                    Name: pidData.Name || '',
                    PID: pidData.PID || '',
                    Expression: pidData.Expression || '',
                    Unit: pidData.Unit || pidData.unit || '',
                    Class: pidData.Class || pidData.class || '',
                    MinValue: pidData.MinValue || '',
                    MaxValue: pidData.MaxValue || '',
                    Period: pidData.Period || '',
                    // Per-PID sweep divisor (issue #29). loadAutoTable hydrates through this
                    // hard-coded whitelist, so a key that is NOT listed here is silently dropped
                    // on load and therefore lost on the next Store -- the rate would revert to
                    // "Every sweep" with no error and nothing in lint or build would catch it.
                    // Deliberately NOT `|| ''`: that idiom coerces a meaningful 0; pass undefined
                    // through and let the row builder normalize absent/0/1/junk.
                    SampleEvery: pidData.SampleEvery,
                    description: pidData.description || '',
                    comment: pidData.comment || '',
                    enabled: pidData.enabled
                });
            });
        }

        // Custom CAN filters (stored in auto_pid.json as top-level can_filters)
        if (Array.isArray(data.can_filters)) {
            data.can_filters.forEach(f => {
                const fid = (f && f.frame_id !== undefined) ? f.frame_id : null;
                const params = (f && Array.isArray(f.parameters)) ? f.parameters : [];
                if (params.length) {
                    params.forEach(param => {
                        addCustomCanFilterEntry({
                            frame_id: fid,
                            parameter: {
                                name: param.name,
                                expression: param.expression,
                                unit: param.unit,
                                class: param.class,
                                period: param.period,
                                type: param.type,
                                min: param.min,
                                max: param.max,
                                description: param.description,
                                comment: param.comment,
                                enabled: param.enabled
                            }
                        });
                    });
                } else if (fid !== null) {
                    addCustomCanFilterEntry({ frame_id: fid, parameter: { name: 'New Parameter', period: '5000' } });
                }
            });
        }

        loadedStdPids = Array.isArray(data.std_pids) ? data.std_pids : [];

        // Calculated channels (Task #17, source CALC)
        if (Array.isArray(data.calculated)) {
            data.calculated.forEach(c => {
                addCalculatedChannelEntry({
                    name: c.name,
                    expression: c.expression,
                    unit: c.unit,
                    enabled: c.enabled
                });
            });
        }

        requestAnimationFrame(() => {
            try {
                if (typeof toggleCarModel === 'function') toggleCarModel();
                if (typeof toggleGroupApiToken === 'function') toggleGroupApiToken();

                togglePidPollingMinVoltageRow();
            } catch (error) {
                console.error('Error in UI updates:', error);
            }
        });
        // Snapshot the just-loaded table as the skip-unchanged baseline for the combined
        // Submit (storeAutoTableData(true)). All .value fields and rows above are set
        // synchronously by here. If the load can't be re-serialized cleanly, leave the
        // baseline null so a Submit always POSTs (safe default).
        try {
            autoTableSavedJson = JSON.stringify(buildAutoTableJson(), null, 0);
        } catch (e) {
            autoTableSavedJson = null;
        }
        // Sample Rate hints (issue #29): one-shot /poll_status read, placed AFTER the
        // autoTableSavedJson snapshot above so sampleLoadCommitted() has its anchor.
        pollSweepRefresh();
        console.log("loadAutoTable completed successfully");

    } catch (error) {
        console.error('Error in loadAutoTable:', error);
        showNotification("Error loading table data: " + error.message, "red");
    }
}

function togglePidPollingMinVoltageRow() {
    const mode = document.getElementById("disable_on_sleep_voltage")?.value || 'automate_threshold';
    const row = document.getElementById("pid_polling_min_voltage_row");
    const warningDiv = document.getElementById("autopid_low_voltage_warning_div");

    if (row) {
        row.style.display = (mode === 'automate_threshold') ? '' : 'none';
    }

    if (warningDiv) {
        warningDiv.style.display = (mode === 'disable') ? 'block' : 'none';
    }
}


function enableAutoStoreButton() {
    const storeButton = document.querySelector('button.store');
    if (storeButton) {
        storeButton.disabled = false;
    }
    document.getElementById("custom_pid_store").disabled = false;
}
    
// Pure metadata (issue #34): description/comment are never read by the engine, and
// an empty field emits no key at all, so no-note configs stay byte-identical.
function emitOptionalNotes(target, entry) {
    const description = entry.querySelector('.description-input')?.value || '';
    if (description) {
        target.description = description;
    }
    const comment = entry.querySelector('.comment-input')?.value || '';
    if (comment) {
        target.comment = comment;
    }
}

// ---------------------------------------------------------------------------------
// Sample Rate (per-PID sweep divisor, issue #29): read-back + the predicted-cadence hint.
// ---------------------------------------------------------------------------------

// Returns an integer >= 0, or NaN when the Custom field holds anything that is not a bare
// non-negative decimal integer (the caller turns NaN into the user-facing throw).
function readSampleEvery(entry) {
    const sel = entry.querySelector('.sample-every-select');
    if (!sel) return 0;                                   // row predates the control
    if (sel.value !== 'custom') return parseInt(sel.value, 10) || 0;
    const raw = (entry.querySelector('.sample-every-custom')?.value || '').trim();
    return /^\d+$/.test(raw) ? parseInt(raw, 10) : NaN;
}

// Effective divisor: 0, 1, and any non-integer/NaN all mean "every sweep" (=1); only an integer
// >=2 actually gates. Single source for the load/min/hint call sites below. readSampleEvery() is
// deliberately NOT normalized here -- the save validator still needs to tell garbage from a real
// divisor -- so the normalizer is a separate one-liner applied only where an effective N is wanted.
const effDivisor = n => (Number.isInteger(n) && n >= 2) ? n : 1;

var pollActive   = false;  // /poll_status .active
var pollQuiesced = false;  // /poll_status .quiesced
var pollWatching = false;  // /poll_status .state === 'watch' (slow sweep, waiting for the engine)
var pollSweepMs  = 0;      // last measured mean sweep, ms; 0 = unknown
var pollReach    = true;   // last fetch succeeded

// Sum of 1/N over ENABLED rows -- the sweep's poll-load, in "PIDs per sweep".
function sampleLoadFromEntries() {
    var sum = 0;
    document.querySelectorAll('.pid-entry').forEach(function (e) {
        if (e.querySelector('.enabled-chk')?.checked === false) return;
        var n = effDivisor(readSampleEvery(e));
        sum += 1 / n;
    });
    return sum;
}

// Same quantity for the config the DEVICE is running. autoTableSavedJson is the last
// committed serialization (snapshotted at load, refreshed after every successful Store),
// so it is exactly the device's table; fall back to the live DOM if it is null.
function sampleLoadCommitted() {
    if (!autoTableSavedJson) return sampleLoadFromEntries();
    try {
        var pids = (JSON.parse(autoTableSavedJson).pids) || [];
        var sum = 0;
        pids.forEach(function (p) {
            if (p.enabled === false) return;
            var n = effDivisor(parseInt(p.SampleEvery, 10));
            sum += 1 / n;
        });
        return sum;
    } catch (e) { return sampleLoadFromEntries(); }
}

// Predicted MEAN sweep for the table AS CURRENTLY EDITED. Anchored to the MEASURED sweep,
// not to rtt_avg_ms: rtt_avg_ms is OK-only and ignores 30 ms timeouts, so an RTT model can
// disagree with the measured figure by ~3x with no user edit. This form is IDENTICALLY the
// measured sweep when nothing has been edited -- the honesty property -- and is an
// approximation (it assumes uniform per-PID cost) only for the delta the user just made.
function predictedSweepMs() {
    if (!pollSweepMs) return 0;
    var base = sampleLoadCommitted();
    var now  = sampleLoadFromEntries();
    if (!(base > 0) || !(now > 0)) return 0;
    return pollSweepMs * (now / base);
}

// Smallest divisor over enabled rows -- the multiplier the firmware's sched_min_n will take.
function currentMinN() {
    var mn = 0;
    document.querySelectorAll('.pid-entry').forEach(function (e) {
        if (e.querySelector('.enabled-chk')?.checked === false) return;
        var n = effDivisor(readSampleEvery(e));
        if (mn === 0 || n < mn) mn = n;
    });
    return mn || 1;
}

// One-shot, no new recurring timer: called on Logger-tab open, at the end of loadAutoTable,
// and 2 s after a successful Store.
function pollSweepRefresh() {
    return fetch('/poll_status')
        .then(function (r) { return r.json(); })
        .then(function (j) {
            pollReach    = true;
            pollActive   = !!(j && j.active === true);
            pollQuiesced = !!(j && j.quiesced === true);
            pollWatching = !!(j && j.state === 'watch');
            pollSweepMs  = (pollActive && Number.isFinite(j.sweep_ms) && j.sweep_ms > 0) ? j.sweep_ms : 0;
        })
        .catch(function () { pollReach = false; pollActive = false; pollWatching = false; pollSweepMs = 0; })
        .then(updateAllSampleHints);
}

// Word the "no number" case from what /poll_status actually says, and ALWAYS render
// something so an ungated row is visibly wired up rather than silently empty.
function sampleUnknownReason() {
    if (!pollReach)   return 'device unreachable';
    if (!pollActive)  return 'not in Datalogger mode';
    if (pollQuiesced) return 'engine off';
    // Watch mode polls slowly on purpose while waiting for the engine, so no fast-sweep rate has
    // been measured yet. Without this the hint would sit on 'measuring…' for as long as the car
    // is parked and read like something is stuck.
    if (pollWatching) return 'waiting for engine';
    return 'measuring…';
}

function sampleHintText(n, predMs) {
    var mult = effDivisor(n);
    var label = (mult === 1) ? 'every sweep' : ('every ' + mult + ' sweeps');
    if (!predMs) return '≈ ' + label + ' (' + sampleUnknownReason() + ')';
    var ms = predMs * mult;
    return '≈ ' + Math.round(ms) + ' ms (' + (1000 / ms).toFixed(1) + ' Hz)';
}

// updateAll..., not updateOne...: changing one row's divisor changes the predicted sweep and
// therefore EVERY row's hint -- that live coupling is the point of the control. Sets
// textContent only and dispatches no event, so it can never mark the table dirty.
function updateAllSampleHints() {
    var pred = predictedSweepMs();
    document.querySelectorAll('.pid-entry').forEach(function (e) {
        var span = e.querySelector('.sample-every-hint');
        if (span) span.textContent = sampleHintText(readSampleEvery(e), pred);
    });
    var ro = document.getElementById('polled_sweep_readout');
    if (!ro) return;
    if (!pollSweepMs) { ro.textContent = 'Measured sweep: not available (' + sampleUnknownReason() + ')'; return; }
    // Warn on the predicted FAST-CHANNEL period, which is what csv_grid_period_ms() clamps
    // at 10 ms (WICAN_LOG_MIN_PERIOD_MS) -- NOT on the predicted sweep. Warning on the sweep false-alarms on every
    // uniformly-gated table (all 19 at N=8 -> 5.9 ms sweep but a 47.5 ms grid period,
    // nowhere near the cap).
    var fastMs = pred * currentMinN();
    var warn = (fastMs > 0 && fastMs < WICAN_LOG_MIN_PERIOD_MS)
        ? '   ⚠ predicted fastest channel is above the ' + WICAN_LOG_MAX_HZ + ' Hz CSV grid cap — the grid will clamp'
        : '';
    ro.textContent = 'Measured sweep: ' + pollSweepMs.toFixed(1) + ' ms (' +
                     (1000 / pollSweepMs).toFixed(1) + ' Hz)' +
                     (pred ? '   ·   predicted after apply: ' + pred.toFixed(1) + ' ms' : '') + warn;
}

// Build the auto_pid.json POST body from the Logger-page DOM. Throws on a validation
// failure (the caller surfaces the message). Split out of storeAutoTableData so the
// same serialization can snapshot a load-time baseline for the skip-unchanged guard.
function buildAutoTableJson() {
    const custom_pid_data = [];
    const custom_can_filters = [];

    const entries = document.querySelectorAll('.pid-entry');

    const initialisationValue = document.getElementById("initialisation")?.value || '';
    const disableOnSleepVoltageValue = document.getElementById("disable_on_sleep_voltage")?.value || 'automate_threshold';
    const pidPollingMinVoltageValueRaw = document.getElementById("pid_polling_min_voltage")?.value;
    const pidPollingMinVoltageValue = (() => {
        const n = parseFloat(pidPollingMinVoltageValueRaw);
        return Number.isFinite(n) ? n : 12.0;
    })();
    if(entries?.length) {
        entries.forEach((entry, index) => {
            const sampleEvery = readSampleEvery(entry);
            // PID box + Mode select -> stored wire string (issue #31; full story at
            // parsePidText). Canonical rows compose service+ident+hint -- the select IS
            // the service source; legacy rows store the box verbatim with Mode derived
            // from its prefix (pre-#31 behavior, never blocked).
            const rowName = entry.querySelector('.name-input')?.value || '';
            const boxText = entry.querySelector('.pid-input')?.value || '';
            const modeSelVal = entry.querySelector('.mode-select')?.value || '01';
            let pidStored, modeStored;
            if (entry.dataset.pidCanonical === '1') {
                // Mode 23 (issue #51) is validated on the two boxes the user actually edits, so the
                // message names the box at fault instead of the 12-hex-char string they never see.
                // Its own check is exhaustive -- address 8 hex + size 1..6 makes rowIdentText
                // return exactly MODE_IDENT_LEN['23'] chars -- so the generic width test below is
                // an `else`, not a follow-up: reached, it would advise "e.g. 0C" for a Mode 23 row.
                if (modeSelVal === RMBA_MODE) {
                    const rmbaSize = parseInt(entry.querySelector('.rmba-size-input')?.value, 10);
                    const why = rmbaIdentProblem(boxText, rmbaSize)
                             || rmbaExprProblem(rowStoredExpr(entry), rmbaSize);
                    if (why) throw new Error(`Mode 23 "${rowName}": ${why}`);
                } else {
                    const wantLen = MODE_IDENT_LEN[modeSelVal];
                    if (!new RegExp('^[0-9A-Fa-f]{' + wantLen + '}$').test(boxText)) {
                        throw new Error(`PID for "${rowName}" must be ${wantLen / 2} byte${wantLen > 2 ? 's' : ''} (${wantLen} hex chars) for Mode ${modeSelVal} — e.g. ${modeSelVal === '22' ? '1746' : '0C'}`);
                    }
                }
                const identText = rowIdentText(entry, modeSelVal);
                pidStored = composePidText(modeSelVal, identText,
                    entry.dataset.pidHint !== undefined ? entry.dataset.pidHint : PID_HINT_DEFAULT);
                modeStored = modeSelVal;
            } else {
                pidStored = boxText;
                modeStored = pidServiceMode(boxText);
                if (!EXPRESSIBLE_MODES.includes(modeStored)) {
                    console.warn(`PID "${rowName}": service "${modeStored}" is outside the Mode dropdown (${EXPRESSIBLE_MODES.join('/')}); saved as-is from the PID text`);
                } else if (modeSelVal !== modeStored) {
                    console.warn(`PID "${rowName}": Mode dropdown is display-only for this non-standard PID shape; storing "${modeStored}" from the text`);
                }
            }
            const pidData = {
                Name: rowName,
                Mode: modeStored,
                PID: pidStored,
                Expression: rowStoredExpr(entry),   // friendly A/B/C -> stored Bn (issue #61)
                Unit: entry.querySelector('.unit-input')?.value || '',
                Class: entry.querySelector('.class-input')?.value || '',
                MinValue: entry.querySelector('.min-value-input')?.value || '',
                MaxValue: entry.querySelector('.max-value-input')?.value || '',
                Period: entry.querySelector('.period-input')?.value || '',
                // Per-PID sweep divisor (issue #29). N<2 is the universal default and the
                // firmware's no-op, so it emits NO key at all -- a config that never touched
                // this control saves byte-identically to what it loaded. Same discipline as
                // emitOptionalNotes() for description/comment; the key sits here, after Period
                // and before enabled, because emitOptionalNotes appends AFTER enabled and would
                // interleave the divisor with description/comment.
                ...(sampleEvery >= 2 ? { SampleEvery: sampleEvery } : {}),
                enabled: entry.querySelector('.enabled-chk')?.checked !== false
            };
            emitOptionalNotes(pidData, entry);

            if (pidData.Name.length === 0 || pidData.Name.length >= 32) {
                throw new Error("Name must not be empty and must be less than 32 characters");
            }
            if (pidData.PID.length === 0 || pidData.PID.length > PID_TEXT_MAX) {
                throw new Error(`PID for "${pidData.Name}" must not be empty and must be at most ${PID_TEXT_MAX} characters`);
            }
            if (pidData.Expression.length === 0 || pidData.Expression.length >= 64) {
                throw new Error("Expression must not be empty and must be less than 64 characters");
            }
            // Period is deliberately NOT validated. It is a retired, hidden field (see the
            // display:none row in the template and the Class+Period note above). The shipping
            // product runs the POLL_LOG protocol, which reads Period NOWHERE -- poll_log sweeps
            // every PID each pass. The only code that reads Period is the legacy AUTO_PID
            // scheduler (a mutually-exclusive protocol slated for removal in #28), and it
            // tolerates a blank value. A brand-new PID row is born with an empty Period the user
            // can neither see nor fix, so the old "Period must be a number greater than 100"
            // check made adding ANY polled PID impossible. The hidden input still round-trips an
            // existing PID's stored value.
            // Sample Rate (issue #29). readSampleEvery returns NaN for a Custom field holding
            // anything that is not a bare non-negative decimal integer, so 4.5 / -2 / 1e3 / ""
            // all land here. Lower bound is 0 in code (0 and 1 are reachable only from the
            // preset select and both mean "every sweep"); the message says 2..64 because that
            // is the range the user can type. The upper bound is a FIRMWARE COUPLING, not a UI
            // preference -- it must stay equal to AUTOPID_MAX_SAMPLE_EVERY in
            // components/autopid/autopid.h, or a saved value would be silently truncated by the
            // parser while the UI kept showing what was typed.
            if (!Number.isInteger(sampleEvery) || sampleEvery < 0 || sampleEvery > 64) {
                throw new Error(`Sample Rate for "${pidData.Name}" must be a whole number from 2 to 64`);
            }
            custom_pid_data.push(pidData);
        });
    }

    // Custom CAN filters: group CONTIGUOUS runs of the same frame_id, not a global
    // Map — a global merge cannot represent an interleaved row order, which made
    // some ▲/▼ moves save byte-identical JSON and silently revert on reload
    // (issue #33). The firmware parses can_filters[] by array index and every
    // matcher walks all entries, so a frame_id split across two runs decodes
    // identically to one merged entry.
    const customFilterEntries = document.querySelectorAll('.custom-canfilter-entry');
    if (customFilterEntries.length > 0) {
        let runKey = null;
        let runGroup = null;
        customFilterEntries.forEach(entry => {
            const fidRaw = entry.querySelector('.frame-id-input')?.value || '';
            const fidNum = normalizeFrameIdInputToNumber(fidRaw);
            const frameIdOut = (fidNum !== null) ? fidNum : String(fidRaw).trim();
            if (!frameIdOut) {
                throw new Error('Broadcast PID Frame ID is required');
            }
            const key = (fidNum !== null) ? `n:${fidNum}` : `s:${String(fidRaw).trim().toLowerCase()}`;
            if (key !== runKey) {
                runKey = key;
                runGroup = { frame_id: frameIdOut, parameters: [] };
                custom_can_filters.push(runGroup);
            }
            const filterParam = {
                name: entry.querySelector('.name-input')?.value || '',
                expression: entry.querySelector('.expression-input')?.value || '',
                unit: entry.querySelector('.unit-input')?.value || '',
                class: entry.querySelector('.class-input')?.value || '',
                period: entry.querySelector('.period-input')?.value || '',
                min: entry.querySelector('.min-input')?.value || '',
                max: entry.querySelector('.max-input')?.value || '',
                enabled: entry.querySelector('.enabled-chk')?.checked !== false
            };
            emitOptionalNotes(filterParam, entry);
            runGroup.parameters.push(filterParam);
        });
    }

    // Calculated channels (Task #17): collect name/expression/unit/enabled rows. Carried
    // through verbatim so a UI "Store" never drops imported calculated channels.
    const calculated_data = [];
    document.querySelectorAll('.calculated-entry').forEach(entry => {
        const name = (entry.querySelector('.name-input')?.value || '').trim();
        const expression = (entry.querySelector('.expression-input')?.value || '').trim();
        const unit = (entry.querySelector('.unit-input')?.value || '').trim();
        const enabled = entry.querySelector('.enabled-chk')?.checked !== false;
        if (!name) return;  // skip unnamed rows
        if (name.length > 47) {
            throw new Error("Calculated PID name must be less than 48 characters");
        }
        calculated_data.push({ name, expression, unit, enabled });
    });

    return {
        initialisation: initialisationValue,
        disable_on_sleep_voltage: disableOnSleepVoltageValue,
        pid_polling_min_voltage: pidPollingMinVoltageValue,
        pids: custom_pid_data,
        // Standard-PIDs keys are passthrough (no UI after issue #21): re-emit the
        // values captured by loadAutoTable so a Store never silently drops them.
        std_pids: loadedStdPids,
        can_filters: custom_can_filters,
        calculated: calculated_data,
        standard_pids: loadedStandardPids,
        ecu_protocol: loadedEcuProtocol
    };
}

// skipIfUnchanged: set by the combined Submit (postConfig), which re-runs this handler
// on every save even from the Settings page. When nothing in the logger/PID table
// changed, skip the POST entirely — it otherwise fires a misleading "PID table applied"
// toast and triggers a needless live PID-table hot-swap (RCU reload) on the poll task.
// The dedicated Logger "Store" button calls with the default (false) and always posts.
async function storeAutoTableData(skipIfUnchanged = false) {
    try {
        const serialized = JSON.stringify(buildAutoTableJson(), null, 0);

        // autoTableSavedJson is snapshotted on load and refreshed after each commit.
        // A null baseline (no clean snapshot) never === the serialized string, so this
        // correctly falls through to always-POST -- no explicit null check needed.
        if (skipIfUnchanged && serialized === autoTableSavedJson) {
            return true;
        }

        await fetch('store_auto_data', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: serialized
        })
        .then(response => response.text())
        .then(result => {
            // Parse the honest envelope {reboot,applied,msg} (issue #39). Store never
            // reboots the device on its own, so do NOT claim "Rebooting" and do NOT lock
            // the buttons. Fall back to legacy plain text if the firmware predates this.
            let msg = "PID table saved.", color = "green";
            try {
                const r = JSON.parse(result);
                if (r && typeof r.msg === "string") msg = r.msg;
                if (r && r.applied === "deferred") color = "yellow";
            } catch (e) {
                if (result) msg = result;   // legacy plain-text firmware
            }
            autoTableSavedJson = serialized;   // committed -> this is the new baseline
            showNotification(msg, color, 6000);
            // Re-measure once the live apply has settled (issue #29): the alpha-1/8 sweep EMA
            // needs ~8 sweeps, and the new baseline above makes the predicted figure collapse
            // back onto the measured one. One setTimeout, not a recurring timer.
            setTimeout(pollSweepRefresh, 2000);
        })
        .catch(error => {
            showNotification("Error saving settings: " + error.message, "red");
            return false;
        });

        return true;

    } catch (error) {
        // safe(): these validation messages quote the row's NAME back at the user, and
        // showNotification assigns innerHTML. A name arrives from an imported .yaml, so an
        // unescaped message is script execution in the device's own origin -- where
        // /store_config, the Wi-Fi credentials and OTA live. Escaped at the sink so every
        // throw in this function is covered, not just today's.
        showNotification(safe(error.message), "red");
        return false;
    }
}

// ---- Sensor-set file: Logger-scoped export / import (issue #63) -------------------
// One file holding every sensor on this page -- polled PIDs, broadcast PIDs and
// calculated channels -- so a tuned set can be backed up, shared, or moved to another
// adapter. Deliberately NOT the System-tab whole-device backup: that one carries
// config.json (Wi-Fi credentials included) and reboots on restore. File format and
// round-trip rules: docs/internals/web_ui.md.
const SENSORS_FILE_KIND = 'sensors';
const SENSORS_FILE_VERSION = 1;
// Period is inert under the Datalogger (only the legacy AutoPID scheduler gates on it), so
// the file omits it and import restores this value -- what every shipped row carries, which
// is what keeps an exported-then-imported config byte-identical to the stored one.
const SENSORS_DEFAULT_PERIOD = '200';
const SENSORS_DEFAULT_CLASS = 'none';   // both restored by the importer; see emitSensorsYaml
// Set once /load_auto_pid has populated the page. Import refuses until then, because the
// settings it passes through are read from the live DOM.
var sensorsPageLoaded = false;

// Blob -> browser download. Shared by the sensor export, the System-tab whole-device
// downloadCfg(), and the Tactrix logcfg.txt exporter to come (issue #37).
function downloadTextFile(filename, text, mime = 'application/json') {
    const url = window.URL.createObjectURL(new Blob([text], { type: mime }));
    const link = document.createElement('a');
    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    window.URL.revokeObjectURL(url);
}

// Every string is quoted, every number and boolean bare. Consistency beats brevity in a
// file people hand-edit: an unquoted "01" would silently become the integer 1, and an
// unquoted % is a YAML directive marker.
const yq = v => '"' + String(v).replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"';

// A row is written in the friendly decomposed form ONLY when its wire string is canonical
// AND carries the default one-response-frame hint. Anything else (exotic service, odd
// shape, a non-default hint) keeps the full wire string, exactly as the UI does -- the
// decomposed form has nowhere to put the difference, and guessing would corrupt the row.
const sensorPidParts = wire => {
    const p = parsePidText(wire);
    return (p && p.hint === PID_HINT_DEFAULT) ? p : null;
};

// Renders what the Logger page SHOWS: Mode + bare identifier, and expressions in the
// standard SAE J1979 / Torque A/B/C vocabulary (issue #61). Derived from
// buildAutoTableJson() rather than re-read from the DOM, so the export runs the same
// validation as Store -- an invalid row throws here instead of producing a file that
// cannot be restored -- and so the friendly/raw decision uses the identical predicates
// (parsePidText, exprAsAbc) the row builder used to decide what to display.
//
// Keys the Datalogger never reads are NOT written: Class (MQTT/HA metadata this firmware
// never publishes), Period (only the legacy AutoPID scheduler gates on it; poll_log sweeps
// every PID), initialisation / ecu_protocol (legacy ELM setup; poll_log hardcodes 0x7E0),
// standard_pids / std_pids. Import restores them at their shipped defaults, so
// auto_pid.json keeps its schema and existing configs still round-trip.
//
// class and period are dropped unconditionally, including the per-row values some configs
// carry (class "temperature", period 1000 on a broadcast row). Writing them only when
// non-default was considered and rejected: they change nothing the logger does, so carrying
// them would add noise to every such row to preserve text nothing reads. They come back as
// SENSORS_DEFAULT_CLASS / SENSORS_DEFAULT_PERIOD, and go for good with issue #28.
function emitSensorsYaml() {
    const stored = buildAutoTableJson();
    const L = [
        '# WiCAN sensor set',
        '#',
        '# Every sensor from the Logger page, written the way the page shows them: Mode plus',
        '# PID identifier, and expressions in the standard OBD form where A is the FIRST DATA',
        '# BYTE of the response (then B, C, ...). Sensors only -- no Wi-Fi credentials and no',
        '# device settings.',
        '#',
        '# Load it back with Import Sensors on the Logger page, review the rows, press Store.',
        '',
        `wican: ${yq(SENSORS_FILE_KIND)}`,
        `version: ${SENSORS_FILE_VERSION}`,
        '',
        '# ---- Polled PIDs ---------------------------------------------------------------',
        '# Requested from the ECU every sweep. mode 01 = standard OBD-II, 22 = extended (DID),',
        '# 23 = memory read, which carries "address" and "size" instead of "pid".',
        '# sample_every omitted means every sweep.',
        'polled:'
    ];
    // One entry: a blank separator, then "- k: v" and aligned continuation lines. The row
    // CONTENTS differ per section; the emit does not, so it lives in one place.
    const emitEntry = (first, row) => {
        if (!first) L.push('');
        row.forEach(([k, v], j) => L.push((j ? '    ' : '  - ') + k + ': ' + v));
    };

    stored.pids.forEach((p, i) => {
        const parts = sensorPidParts(p.PID);
        // rangeToArith FIRST, exactly as the row builder does before it classifies (issue #61),
        // or the file would print the compact [A:D] for a stored [B2:B5] while the page shows
        // ((A*16777216)+...) for the same row -- the one thing this format promises not to do.
        // Both forms import to the same bytes; a Store then persists the arithmetic form, which
        // is the same one-time migration an ordinary load->Store already performs.
        const abc = parts ? exprAsAbc(rangeToArith(p.Expression), exprDataOffset(parts.service)) : null;
        const row = [['name', yq(p.Name)], ['mode', yq(parts ? parts.service : pidServiceMode(p.PID))]];
        // Mode 23 carries address + size, the same two ideas the page edits, rather than the
        // glued 12-hex-char identifier nobody types (issue #51).
        if (parts && parts.service === RMBA_MODE) {
            const sp = rmbaSplitIdent(parts.ident);
            row.push(['address', yq(sp.addr)], ['size', sp.size]);
        } else {
            row.push(['pid', yq(parts ? parts.ident : p.PID)]);
        }
        row.push(['expression', yq(abc !== null ? abc : p.Expression)], ['unit', yq(p.Unit)]);
        if (p.SampleEvery) row.push(['sample_every', p.SampleEvery]);
        row.push(['enabled', p.enabled !== false]);
        if (p.description) row.push(['description', yq(p.description)]);
        if (p.comment) row.push(['comment', yq(p.comment)]);
        if (p.MinValue) row.push(['min', yq(p.MinValue)]);
        if (p.MaxValue) row.push(['max', yq(p.MaxValue)]);
        emitEntry(i === 0, row);
    });

    L.push('', '# ---- Broadcast PIDs ------------------------------------------------------------',
           '# Decoded from frames the bus already sends -- nothing is requested, so there is no',
           '# request echo to skip and bytes are referenced directly as B0..B7.',
           'broadcast:');
    let n = 0;
    (stored.can_filters || []).forEach(f => (f.parameters || []).forEach(prm => {
        // formatFrameIdForUi is what the Frame ID box itself renders -- reuse it rather than
        // hand-rolling a second hex formatter, which zero-padded to 3 digits and so wrote
        // 0x0F0 where the page shows 0xF0.
        const fid = (typeof f.frame_id === 'number') ? formatFrameIdForUi(f.frame_id)
                                                     : yq(f.frame_id);
        const row = [['frame_id', fid], ['name', yq(prm.name)],
                     ['expression', yq(prm.expression)], ['unit', yq(prm.unit)],
                     ['enabled', prm.enabled !== false]];
        if (prm.description) row.push(['description', yq(prm.description)]);
        if (prm.comment) row.push(['comment', yq(prm.comment)]);
        if (prm.min) row.push(['min', yq(prm.min)]);
        if (prm.max) row.push(['max', yq(prm.max)]);
        emitEntry(n++ === 0, row);
    }));

    L.push('', '# ---- Calculated PIDs -----------------------------------------------------------',
           '# Derived from other channels by name, evaluated after each sweep.',
           'calculated:');
    (stored.calculated || []).forEach((c, i) => {
        emitEntry(i === 0, [['name', yq(c.name)], ['expression', yq(c.expression)],
                            ['unit', yq(c.unit)], ['enabled', c.enabled !== false]]);
    });
    return L.join('\n') + '\n';
}

function exportSensors() {
    try {
        downloadTextFile(`wican_sensors_${new Date().toISOString().split('T')[0]}.yaml`,
                         emitSensorsYaml(), 'text/yaml');
        showNotification("Sensor set exported.", "green");
    } catch (error) {
        // Validation messages quote row names, which may have come from an imported file.
        showNotification("Export failed: " + safe(error.message), "red");
    }
}

function importSensors() {
    const fileInput = document.getElementById('sensors_file');
    const file = fileInput?.files?.[0];
    if (!file) return;

    const reader = new FileReader();
    reader.onload = function(e) {
        try {
            applySensorsFile(e.target.result);
        } catch (error) {
            // safe(): showNotification assigns innerHTML, and parse errors quote the
            // offending LINE back to the user. A shared sensor file is untrusted input --
            // an unescaped <img onerror=...> in it would otherwise run script in the
            // device's own origin, where /store_config and OTA live.
            showNotification("Import failed: " + safe(error.message), "red");
        }
        fileInput.value = '';   // clear, or re-picking the same file fires no change event
    };
    reader.onerror = function() {
        showNotification("Import failed: could not read that file.", "red");
        fileInput.value = '';
    };
    reader.readAsText(file);
}

// Reads exactly the shape emitSensorsYaml() writes: top-level scalars plus three block
// sequences of flat key/value maps. Deliberately NOT a general YAML implementation -- a
// full parser is tens of KB in a main.js that ships uncompressed inside the firmware
// image, and the constructs it would add (anchors, flow collections, multi-line scalars)
// are ones this format never emits. Anything outside the shape is REJECTED with a line
// number, because silently guessing at a hand edit would reshape someone's sensor set.
const SENSORS_YAML_SECTIONS = ['polled', 'broadcast', 'calculated'];

function parseSensorsScalar(text, lineNo) {
    const fail = m => { throw new Error(`line ${lineNo}: ${m}`); };
    if (text.startsWith('"')) {
        let out = '', j = 1, closed = false;
        while (j < text.length) {
            if (text[j] === '\\' && j + 1 < text.length) { out += text[j + 1]; j += 2; continue; }
            if (text[j] === '"') { closed = true; j++; break; }
            out += text[j++];
        }
        if (!closed) fail('unterminated quoted value (missing closing ")');
        const tail = text.slice(j).trim();
        if (tail && !tail.startsWith('#')) fail(`unexpected text after the quoted value: ${tail}`);
        return out;
    }
    const hash = text.indexOf(' #');
    const v = (hash >= 0 ? text.slice(0, hash) : text).trim();
    if (!v) fail('missing value');
    if (v === 'true' || v === 'false') return v === 'true';
    // 0x... is deliberately NOT coerced to a number here. Only the caller knows whether a
    // hex token is a VALUE (frame_id 0x240 -> 576) or DIGITS (pid 0x16CD -> the identifier
    // "16CD"); coercing centrally turned a hand-written `pid: 0x16CD` into 5837, whose
    // decimal text "5837" then passed the 4-hex-digit test and polled the wrong DID.
    if (/^-?\d+$/.test(v)) return parseInt(v, 10);
    // No float branch: nothing this format emits is a bare decimal, and the only field a
    // hand-written one reaches (min/max) is String()'d straight back -- parsing it would
    // only rewrite "1.50" as "1.5". Bare strings pass through verbatim instead.
    return v;   // bare string: tolerated on a hand edit even though we always quote
}

function parseSensorsYaml(text) {
    const doc = {};
    let list = null, item = null;
    String(text).split(/\r\n|\r|\n/).forEach((raw, idx) => {
        const lineNo = idx + 1;
        const fail = m => { throw new Error(`line ${lineNo}: ${m}`); };
        const line = raw.replace(/\s+$/, '');
        if (!line.trim() || line.trim().startsWith('#')) return;
        if (/^ *\t/.test(line)) fail('tab used for indentation -- YAML requires spaces');

        const indent = line.length - line.trimStart().length;
        let body = line.trim();
        const isItem = body.startsWith('- ');
        if (isItem) body = body.slice(2).trim();

        const m = /^([A-Za-z_][A-Za-z0-9_]*)\s*:(.*)$/.exec(body);
        if (!m) fail(`expected "key: value", got: ${body}`);
        const key = m[1], rest = m[2].trim();

        if (!isItem && indent === 0) {
            // Last-wins would silently DISCARD everything under the first occurrence --
            // exactly the "quietly reshape a sensor set" outcome this parser exists to
            // prevent, and the likeliest way a hand edit loses rows (splitting the polled
            // list into two blocks).
            if (key in doc) fail(`"${key}" appears more than once at the top level`);
            if (rest === '') {
                if (!SENSORS_YAML_SECTIONS.includes(key)) {
                    fail(`unknown section "${key}" (expected ${SENSORS_YAML_SECTIONS.join(', ')})`);
                }
                list = doc[key] = [];
                item = null;
            } else {
                doc[key] = parseSensorsScalar(rest, lineNo);
                list = null;
                item = null;
            }
            return;
        }
        if (isItem) {
            if (!list) fail('list entry outside any section');
            item = {};
            list.push(item);
        } else if (!item) {
            fail(`"${key}" is indented but not inside a list entry`);
        } else if (key in item) {
            fail(`"${key}" is set twice in the same entry`);
        }
        item[key] = parseSensorsScalar(rest, lineNo);
    });
    return doc;
}

// Every field the file may omit lands as the empty string; `undefined` must not reach the
// row builders as the literal "undefined".
const sTxt = v => v === undefined ? '' : String(v);

// Rebuilds the auto_pid shape loadAutoTable() hydrates from, restoring at their shipped
// defaults every key the file deliberately omits. The file is a VIEW of the sensor set,
// not a copy of auto_pid.json, so the stored schema is reconstructed here rather than
// carried around in the file where nobody can read it.
function sensorsFileToAutoPid(doc) {
    const pids = (doc.polled || []).map((r, i) => {
        const where = r.name ? `polled PID "${r.name}"` : `polled PID #${i + 1}`;
        if (!r.name) throw new Error(`${where} has no name.`);
        // We always WRITE mode and pid quoted, but a hand editor may not. Two ways an
        // unquoted edit arrives wrong, both fixed here rather than in the row builder:
        //   `mode: 01` / `pid: 03`   -> YAML number 1 / 3, needing the leading zero back
        //   `pid: 0x16CD`            -> hex DIGITS, not a value (the 0x form is natural
        //                               here because frame_id in the same file uses it)
        // Left unhandled, either silently fails the canonical test below and stores an
        // untranslated wire string -- a wrong PID with no error anywhere.
        const hexDigits = v => String(v).replace(/^0[xX]/, '');
        const mode = hexDigits(r.mode === undefined ? '01' : r.mode).padStart(2, '0');
        const identLen = MODE_IDENT_LEN[mode];
        let pid;
        // Branch on the KEY PRESENT, not on the mode alone. A mode-23 row the export could not
        // decompose -- a wire string with no frames-hint nibble, or a non-default one -- is
        // written as a verbatim `pid:` exactly like an exotic mode 01/22 row, and must import
        // back the same way. Keying purely on mode made the page reject its own export, and
        // fail the WHOLE file while naming an `address` key the file did not contain.
        // ...but a row carrying NEITHER key is a genuine mistake, so it still gets the specific
        // "needs an address" message rather than falling through to an empty PID that only
        // fails later, at Store, with a vaguer complaint.
        if (mode === RMBA_MODE && (r.address !== undefined || r.pid === undefined)) {
            // Mode 23 is authored as address + size (issue #51), matching the page and the
            // export. Deliberately NOT zero-padded the way a short numeric pid is above: a
            // half-written address is a DIFFERENT address, so it is an error, not a guess.
            const addr = hexDigits(sTxt(r.address));
            const size = parseInt(r.size, 10);
            const why = rmbaIdentProblem(addr, size);
            if (why) throw new Error(`${where}: ${why}.`);
            pid = rmbaJoinIdent(addr, size);
        } else {
            pid = hexDigits(sTxt(r.pid));
            if (identLen !== undefined && /^\d+$/.test(pid) && pid.length < identLen) {
                pid = pid.padStart(identLen, '0');
            }
        }
        // Decomposed only when the identifier is exactly the width this Mode declares;
        // anything else is taken as a complete wire string, matching the export.
        const canonical = identLen !== undefined && new RegExp(`^[0-9A-Fa-f]{${identLen}}$`).test(pid);
        // An expression is already in wire form when a token names a numbered byte (B3, S2)
        // -- friendly A/B/C names are single letters and can never look like that. Same
        // predicate exprAsAbc uses to decide which rows may be shown friendly (issue #61).
        const expr = sTxt(r.expression);
        return {
            Name: String(r.name),
            PID: canonical ? composePidText(mode, pid, PID_HINT_DEFAULT) : pid,
            Expression: (canonical && !exprHasWireByte(expr))
                ? abcToBn(expr, exprDataOffset(mode)) : expr,
            Unit: sTxt(r.unit),
            Class: SENSORS_DEFAULT_CLASS,
            MinValue: sTxt(r.min),
            MaxValue: sTxt(r.max),
            Period: SENSORS_DEFAULT_PERIOD,
            SampleEvery: r.sample_every,
            description: sTxt(r.description),
            comment: sTxt(r.comment),
            enabled: r.enabled !== false
        };
    });
    // One group per row: loadAutoTable() builds one DOM row per parameter either way, and
    // buildAutoTableJson() re-groups consecutive same-frame rows on Store, so the stored
    // grouping is reproduced from row ORDER without this having to replicate that logic.
    const can_filters = (doc.broadcast || []).map((r, i) => {
        if (r.frame_id === undefined) throw new Error(`broadcast PID #${i + 1} has no frame_id.`);
        // Same normalization the Frame ID box performs, so 0x240 / "0x240" / 240 / "7E8" all
        // mean here exactly what they mean when typed into the UI. null (unparseable) falls
        // through verbatim for the row builder to display and the user to correct.
        const fid = normalizeFrameIdInputToNumber(r.frame_id);
        return {
            frame_id: fid === null ? r.frame_id : fid,
            parameters: [{
                name: sTxt(r.name),
                expression: sTxt(r.expression),
                unit: sTxt(r.unit),
                class: SENSORS_DEFAULT_CLASS,
                period: SENSORS_DEFAULT_PERIOD,
                min: sTxt(r.min),
                max: sTxt(r.max),
                description: sTxt(r.description),
                comment: sTxt(r.comment),
                enabled: r.enabled !== false
            }]
        };
    });
    const calculated = (doc.calculated || []).map((r, i) => {
        // buildAutoTableJson() silently skips an unnamed calculated row, so an unnamed one
        // here would vanish at Store with nothing said. Refuse it up front instead.
        if (!r.name) throw new Error(`calculated PID #${i + 1} has no name.`);
        return {
            name: String(r.name),
            expression: sTxt(r.expression),
            unit: sTxt(r.unit),
            enabled: r.enabled !== false
        };
    });
    // auto_pid.json keys the file deliberately does not carry keep whatever the DEVICE
    // already has. This is a sensor set, not a device config: importing one must never move
    // a voltage threshold. Read BEFORE loadAutoTable() runs, because it would otherwise
    // apply its own absent-key defaults -- silently flipping disable_on_sleep_voltage to
    // "disable" and blanking initialisation.
    return {
        initialisation: document.getElementById('initialisation')?.value || '',
        disable_on_sleep_voltage: document.getElementById('disable_on_sleep_voltage')?.value
                                  || 'automate_threshold',
        pid_polling_min_voltage: document.getElementById('pid_polling_min_voltage')?.value,
        standard_pids: loadedStandardPids,
        ecu_protocol: loadedEcuProtocol,
        std_pids: loadedStdPids,
        pids, can_filters, calculated
    };
}

// Loads the file into the tables for REVIEW. Nothing reaches the device here -- the user
// looks the rows over and presses Store, which runs the same validation and the same
// /store_auto_data path as any hand edit. Throws (caught by the caller) on a file this
// page cannot read, so a wrong pick is never a silent no-op.
function applySensorsFile(text) {
    // The import carries over the logger settings the file omits by reading them off the
    // page (see sensorsFileToAutoPid). Before /load_auto_pid has come back those elements
    // still hold the static HTML defaults, so importing into a half-loaded page would
    // capture a default voltage threshold and push it on the next Store -- the one thing
    // omitting those keys is supposed to prevent.
    if (!sensorsPageLoaded) {
        throw new Error("the page is still loading the current sensor set. Try again in a moment.");
    }
    if (/^\s*[{[]/.test(text)) {
        // The pre-release JSON format and the whole-device backup both start this way.
        throw new Error("that looks like a JSON file. Sensor sets are YAML (.yaml) — export a fresh one, and use System → Upload Configuration for a full device backup.");
    }
    const doc = parseSensorsYaml(text);   // throws with a line number
    if (doc.wican !== SENSORS_FILE_KIND) {
        throw new Error("that file is not a WiCAN sensor file (no `wican: \"sensors\"` line).");
    }
    // Only a readable-but-too-new file earns "update the firmware"; a missing or junk
    // version means the marker was a coincidence, not a file from a newer build.
    const version = Number(doc.version);
    if (!Number.isFinite(version)) {
        throw new Error("that file is not a WiCAN sensor file (no readable `version`).");
    }
    if (version > SENSORS_FILE_VERSION) {
        throw new Error(`that sensor file is version ${version}; this firmware reads up to ${SENSORS_FILE_VERSION}. Update the firmware.`);
    }
    if (!SENSORS_YAML_SECTIONS.some(s => Array.isArray(doc[s]))) {
        throw new Error("that sensor file carries no sensor data.");
    }
    const autoPid = sensorsFileToAutoPid(doc);

    // autoTableSavedJson means one thing: the table the DEVICE is running. loadAutoTable()
    // re-snapshots it from whatever it just loaded, which would break both consumers --
    // the combined Submit's skipIfUnchanged path would read the import as "nothing
    // changed" and never POST it, and sampleLoadCommitted() would anchor the "predicted
    // after apply" sweep to the imported set itself, claiming the device already runs it.
    // Restoring the pre-import value keeps that single meaning; pollSweepRefresh() is
    // re-run because loadAutoTable() already fired it against the clobbered baseline.
    const deviceBaseline = autoTableSavedJson;
    loadAutoTable(autoPid);   // replaces all three tables -- see the resets there
    autoTableSavedJson = deviceBaseline;
    pollSweepRefresh();
    enableAutoStoreButton();
    submit_enable();

    const n = sel => document.querySelectorAll(sel).length;
    showNotification(`Imported ${n('.pid-entry')} polled, ${n('.custom-canfilter-entry')} broadcast, ` +
                     `${n('.calculated-entry')} calculated. Review the rows, then press <b>Store</b> to apply them.`,
                     "blue", 8000);
}

var filesCwd = '';   // current directory, relative to /sdcard
var filesSortKey = 'mtime';   // 'name' | 'size' | 'type' | 'mtime'
var filesSortDir = 'desc';    // 'asc' | 'desc' (default: newest first)

function filesFmtSize(b) {
    if (b == null) return '';
    if (b < 1024) return b + ' B';
    if (b < 1048576) return (b / 1024).toFixed(1) + ' KB';
    if (b < 1073741824) return (b / 1048576).toFixed(1) + ' MB';
    return (b / 1073741824).toFixed(2) + ' GB';
}

function filesFmtDate(mtime) {
    if (!mtime) return '';
    var d = new Date(mtime * 1000);
    if (isNaN(d.getTime())) return '';
    function p(n) { return (n < 10 ? '0' : '') + n; }
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) +
           ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
}

function filesJoin(dir, name) { return dir ? (dir + '/' + name) : name; }

function filesParent(dir) {
    if (!dir) return '';
    var i = dir.lastIndexOf('/');
    return i < 0 ? '' : dir.substring(0, i);
}

function filesApi(method, qsOrBody, cb) {
    var xhr = new XMLHttpRequest();
    if (method === 'GET') {
        xhr.open('GET', '/files?' + qsOrBody);
    } else {
        xhr.open('POST', '/files');
        xhr.setRequestHeader('Content-Type', 'application/json');
    }
    xhr.onreadystatechange = function() {
        if (xhr.readyState === 4) {
            var ok = xhr.status >= 200 && xhr.status < 300;
            var data = null;
            try { data = JSON.parse(xhr.responseText); } catch (e) {}
            cb(ok, data, xhr.status);
        }
    };
    xhr.send(method === 'GET' ? null : qsOrBody);
}

function filesLoad(rel) {
    filesCwd = rel || '';
    filesApi('GET', 'op=list&path=' + encodeURIComponent(filesCwd), function(ok, data) {
        if (!ok || !data) {
            var b = document.getElementById('files_body');
            if (b) b.innerHTML = '<tr><td colspan=6 style="text-align:center;color:#b91c1c;padding:8px">Failed to load folder</td></tr>';
            return;
        }
        filesRender(data);
    });
    filesApi('GET', 'op=df', function(ok, data) {
        var el = document.getElementById('files_df');
        if (!el) return;
        el.textContent = (ok && data && data.total) ? ('SD: ' + filesFmtSize(data.free) + ' free of ' + filesFmtSize(data.total)) : '';
    });
}

// Update the sortable header labels (Name/Size/Modified/Type) with the active arrow.
function filesUpdateSortHeaders() {
    var keys = ['name', 'size', 'mtime', 'type'];
    var labels = { name: 'Name', size: 'Size', mtime: 'Modified', type: 'Type' };
    keys.forEach(function(k) {
        var th = document.getElementById('files_th_' + k);
        if (!th) return;
        th.textContent = labels[k] + (filesSortKey === k ? (filesSortDir === 'asc' ? ' ▲' : ' ▼') : '');
    });
}

// Header click handler: toggle direction on the active column, else switch column.
function filesSortBy(key) {
    if (filesSortKey === key) {
        filesSortDir = (filesSortDir === 'asc') ? 'desc' : 'asc';
    } else {
        filesSortKey = key;
        filesSortDir = (key === 'mtime') ? 'desc' : 'asc';
    }
    // Preserve the current checkbox selection across the sort re-render.
    if (filesLastData) filesRender(filesLastData, filesSelectedPaths());
}

var filesLastData = null;

function filesRender(data, keepSel) {
    filesLastData = data;
    // The DOM checkboxes are the sole source of truth for selection. keepSel is an
    // optional array of rel-paths whose checkbox should stay ticked across a re-render
    // (used by sort, which rebuilds the rows); a fresh folder load passes nothing.
    var keep = {};
    if (keepSel) { for (var ki = 0; ki < keepSel.length; ki++) { keep[keepSel[ki]] = true; } }
    document.getElementById('files_path').textContent = '/sdcard' + (data.path ? '/' + data.path : '');
    var body = document.getElementById('files_body');
    body.innerHTML = '';
    var selAll = document.getElementById('files_selall');
    if (selAll) selAll.checked = false;
    filesUpdateSortHeaders();
    if (data.sd_mounted === false) {
        body.innerHTML = '<tr><td colspan=6 style="text-align:center;color:#b91c1c;padding:8px">SD card not mounted</td></tr>';
        return;
    }
    // Cell styling lives in the #files_table CSS rules (so media queries can shrink padding);
    // cells are created bare here.
    if (filesCwd) {
        var up = document.createElement('tr');
        var uc = document.createElement('td'); uc.colSpan = 6;
        var ua = document.createElement('a'); ua.href = '#'; ua.textContent = '.. (up one level)';
        ua.onclick = function(e) { e.preventDefault(); filesLoad(filesParent(filesCwd)); };
        uc.appendChild(ua); up.appendChild(uc); body.appendChild(up);
    }
    var entries = (data.entries || []).slice();
    var dir = (filesSortDir === 'asc') ? 1 : -1;
    function cmp(a, b) {
        var r = 0;
        if (filesSortKey === 'size') {
            r = (a.size || 0) - (b.size || 0);
        } else if (filesSortKey === 'mtime') {
            r = (a.mtime || 0) - (b.mtime || 0);
        } else if (filesSortKey === 'type') {
            r = (a.type || '').localeCompare(b.type || '');
        } else {
            r = a.name.localeCompare(b.name);
        }
        if (r === 0) r = a.name.localeCompare(b.name);
        return r * dir;
    }
    entries.sort(function(a, b) {
        // Keep directories grouped first; sort chosen key within each group.
        if ((a.type === 'dir') !== (b.type === 'dir')) return a.type === 'dir' ? -1 : 1;
        return cmp(a, b);
    });
    entries.forEach(function(en) {
        var isDir = en.type === 'dir';
        var rel = filesJoin(filesCwd, en.name);
        var tr = document.createElement('tr');

        var selTd = document.createElement('td');
        if (!isDir) {
            var cb = document.createElement('input'); cb.type = 'checkbox';
            cb.className = 'files_sel_cb'; cb.value = rel; cb.checked = !!keep[rel];
            selTd.appendChild(cb);
        }
        tr.appendChild(selTd);

        var nameTd = document.createElement('td');
        if (isDir) {
            var a = document.createElement('a'); a.href = '#'; a.textContent = en.name + '/';
            a.onclick = function(e) { e.preventDefault(); filesLoad(rel); };
            nameTd.appendChild(a);
        } else {
            nameTd.textContent = en.name + (en.active ? '  (active log)' : '');
        }
        tr.appendChild(nameTd);

        var sizeTd = document.createElement('td');
        sizeTd.textContent = isDir ? '' : filesFmtSize(en.size); tr.appendChild(sizeTd);

        var dateTd = document.createElement('td');
        dateTd.textContent = filesFmtDate(en.mtime); tr.appendChild(dateTd);

        var typeTd = document.createElement('td');
        typeTd.textContent = isDir ? 'folder' : 'file'; tr.appendChild(typeTd);

        var actTd = document.createElement('td');
        if (!isDir) {
            var dl = document.createElement('button'); dl.textContent = 'Download'; dl.style.marginRight = '4px';
            dl.onclick = function() { filesDownload(rel, en.name); };
            actTd.appendChild(dl);
        }
        if (!en.locked) {
            var rn = document.createElement('button'); rn.textContent = 'Rename'; rn.style.marginRight = '4px';
            rn.onclick = function() { filesRename(rel, en.name); };
            actTd.appendChild(rn);
            var del = document.createElement('button'); del.textContent = 'Delete';
            del.onclick = function() { filesDelete(rel, en.name, isDir); };
            actTd.appendChild(del);
        }
        tr.appendChild(actTd);

        body.appendChild(tr);
    });
    if (!entries.length) {
        var er = document.createElement('tr'); var ec = document.createElement('td');
        ec.colSpan = 6; ec.style.cssText = 'text-align:center;color:#555;padding:8px'; ec.textContent = '(empty)';
        er.appendChild(ec); body.appendChild(er);
    }
}

// Header "select all" checkbox: toggle every currently-listed file checkbox.
function filesToggleAll(cb) {
    var boxes = document.getElementsByClassName('files_sel_cb');
    for (var i = 0; i < boxes.length; i++) {
        boxes[i].checked = cb.checked;
    }
}

// Collect the relative paths of all currently-checked file rows.
function filesSelectedPaths() {
    var out = [];
    var boxes = document.getElementsByClassName('files_sel_cb');
    for (var i = 0; i < boxes.length; i++) {
        if (boxes[i].checked) out.push(boxes[i].value);
    }
    return out;
}

function filesDownloadSelected() {
    var paths = filesSelectedPaths();
    if (!paths.length) { alert('No files selected.'); return; }
    // No zip endpoint: trigger each single-file download with a small stagger.
    var i = 0;
    function next() {
        if (i >= paths.length) return;
        var rel = paths[i++];
        var name = rel.indexOf('/') >= 0 ? rel.substring(rel.lastIndexOf('/') + 1) : rel;
        filesDownload(rel, name);
        setTimeout(next, 400);
    }
    next();
}

function filesDeleteSelected() {
    var paths = filesSelectedPaths();
    if (!paths.length) { alert('No files selected.'); return; }
    if (!confirm('Delete ' + paths.length + ' selected file(s)?')) return;
    var remaining = paths.length;
    var failures = [];
    paths.forEach(function(rel) {
        filesApi('POST', JSON.stringify({ op: 'delete', path: rel }), function(ok, data, st) {
            if (!ok) failures.push(rel + ': ' + ((data && data.error) || st));
            if (--remaining === 0) {
                if (failures.length) alert('Some deletes failed:\n' + failures.join('\n'));
                filesLoad(filesCwd);
            }
        });
    });
}

function filesDownload(rel, name) {
    var a = document.createElement('a');
    a.href = '/files?op=download&path=' + encodeURIComponent(rel);
    a.download = name;
    document.body.appendChild(a); a.click(); document.body.removeChild(a);
}

function filesMkdir() {
    var name = prompt('New folder name:');
    if (!name) return;
    filesApi('POST', JSON.stringify({ op: 'mkdir', path: filesCwd, name: name }), function(ok, data, st) {
        if (!ok) alert('Create failed: ' + ((data && data.error) || st));
        filesLoad(filesCwd);
    });
}

function filesRename(rel, oldName) {
    var name = prompt('Rename "' + oldName + '" to:', oldName);
    if (!name || name === oldName) return;
    filesApi('POST', JSON.stringify({ op: 'rename', path: rel, name: name }), function(ok, data, st) {
        if (!ok) alert('Rename failed: ' + ((data && data.error) || st));
        filesLoad(filesCwd);
    });
}

function filesDelete(rel, name, isDir) {
    if (!confirm('Delete ' + (isDir ? 'folder (and ALL its contents)' : 'file') + ' "' + name + '"?')) return;
    filesApi('POST', JSON.stringify({ op: 'delete', path: rel }), function(ok, data, st) {
        if (!ok) alert('Delete failed: ' + ((data && data.error) || st));
        filesLoad(filesCwd);
    });
}

// Mobile hamburger drawer (<=640px): toggle the .sidebar as an off-canvas panel + backdrop.
// All no-ops on desktop, where the drawer CSS never applies and the elements stay hidden.
function openDrawer() {
    var s = document.querySelector('.sidebar');
    var b = document.getElementById('drawer_backdrop');
    if (s) s.classList.add('open');
    if (b) b.classList.add('open');
}
function closeDrawer() {
    var s = document.querySelector('.sidebar');
    var b = document.getElementById('drawer_backdrop');
    if (s) s.classList.remove('open');
    if (b) b.classList.remove('open');
}
function toggleDrawer() {
    var s = document.querySelector('.sidebar');
    if (s && s.classList.contains('open')) closeDrawer(); else openDrawer();
}

function openTab(evt, tabName) {
    var i, tabcontent, tablinks;
    tabcontent = document.getElementsByClassName("tabcontent");
    for(i = 0; i < tabcontent.length; i++) {
        tabcontent[i].style.display = "none";
    }
    tablinks = document.getElementsByClassName("tablinks");
    for(i = 0; i < tablinks.length; i++) {
        tablinks[i].className = tablinks[i].className.replace(" active", "");
    }
    // Stop the CSV status poll on every tab switch (restarted below only for the logger tab).
    if (typeof csv_status_poll_stop === 'function') csv_status_poll_stop();
    document.getElementById(tabName).style.display = "block";
    evt.currentTarget.className += " active";
    // Close the mobile drawer after a tab is chosen (no-op on desktop); keep the active tab in view.
    if (typeof closeDrawer === 'function') closeDrawer();
    if (evt.currentTarget.scrollIntoView) evt.currentTarget.scrollIntoView({ block: 'nearest' });

    if (tabName === 'files_tab') {
        filesCwd = '';
        filesLoad('');
    } else if (tabName === 'console_tab') {
        csv_status_poll_start();
        consoleRefresh();
    } else if (tabName === 'logger') {
        // Refresh the measured sweep behind the Sample Rate hints (issue #29). One-shot.
        pollSweepRefresh();
    }
}

function sta_enable() {}

// Helper function to get DOM elements efficiently
function getElements() {
    return {
        wifiMode: document.getElementById("wifi_mode"),
        wifiScanButton: document.getElementById("wifi_scan_button"),
        ssidValue: document.getElementById("ssid_value"),
        passValue: document.getElementById("pass_value"),
        staSecurity: document.getElementById("sta_security"),
        bleStatus: document.getElementById("ble_status"),
        apAutoDisable: document.getElementById("ap_auto_disable"),
        bleWarningDiv: document.getElementById("ble_warning_div"),
        battAlert: document.getElementById("batt_alert"),
        battAlertDiv: document.getElementById("batt_alert_div"),
        submitButton: document.getElementById("submit_button"),
        apPassValue: document.getElementById("ap_pass_value"),
        tcpPortValue: document.getElementById("tcp_port_value"),
        battAlertPort: document.getElementById("batt_alert_port"),
        blePassValue: document.getElementById("ble_pass_value"),
        sleepVolt: document.getElementById("sleep_volt"),
        sleepStatus: document.getElementById("sleep_status"),
        sleepDisableAgree: document.getElementById("sleep_disable_agree"),
        protocol: document.getElementById("protocol"),
        portType: document.getElementById("port_type"),
        sta_ble_info: document.getElementById("sta_ble_info")
    };
}

// Helper function to validate field length
function validateLength(value, min, max, fieldName) {
    const length = value.length;
    return length >= min && length <= max;
}

// Helper function to validate port number
function validatePort(value) {
    const port = parseInt(value);
    return port >= 1 && port <= 65535;
}

// Helper function to disable submit button with error message
function disableSubmitWithError(message, duration = 5000) {
    showNotification(message, "red", duration);
    return false;
}

function submit_enable() {
    console.log("submit_enable");
    const elements = getElements();
    const wifiMode = elements.wifiMode.value;
    
    // Configure WiFi mode-specific settings
    configureWifiModeSettings(elements, wifiMode);
    
    // Handle BLE status and warnings
    handleBleStatus(elements);
    
    // Validate form and enable/disable submit button
    const isValid = validateForm(elements, wifiMode);
    elements.submitButton.disabled = !isValid;
    
    // Configure protocol-specific settings
    configureProtocolSettings(elements);
    
    // Configure sleep and battery alert settings
    configureSleepSettings(elements);
    
    // Configure MQTT and battery alert visibility
    configureMqttAndBatteryAlerts(elements);
}

function configureWifiModeSettings(elements, wifiMode) {
    const isAP = wifiMode === "AP";
    const isAPStation = wifiMode === "APStation";
    const isStation = wifiMode === "Station";
    const usesAP = isAP || isAPStation;
    const apChValue = document.getElementById("ap_ch_value");

    // Set station fields
    elements.ssidValue.disabled = isAP;
    elements.passValue.disabled = isAP;
    elements.staSecurity.disabled = isAP;
    elements.wifiScanButton.disabled = isAP;

    // Set AP fields (Station-only does not run AP)
    if (apChValue) apChValue.disabled = !usesAP;
    if (elements.apPassValue) elements.apPassValue.disabled = !usesAP;

    // Auto-disable AP only applies to AP+Station
    elements.apAutoDisable.disabled = !isAPStation;

    // BLE element state (the BLE section is hidden but still round-trips) + station info note
    if (isAP) {
        elements.bleStatus.disabled = false;
        elements.blePassValue.disabled = false;
        elements.sta_ble_info.style.display = "none";
    } else if (isStation) {
        elements.sta_ble_info.style.display = "block";
    } else {   // APStation
        elements.bleStatus.disabled = true;
        elements.bleStatus.value = "disable";
        elements.bleStatus.selectedIndex = 1;
        elements.blePassValue.disabled = true;
        elements.sta_ble_info.style.display = "none";
    }
}

function handleBleStatus(elements) {
    const isBleEnabled = elements.bleStatus.value === "enable";

    elements.bleWarningDiv.style.display = isBleEnabled ? "block" : "none";
    // Enable BLE passkey input only when BLE is enabled
    elements.blePassValue.disabled = !isBleEnabled;
    
    if (isBleEnabled && !window.bleAlertShown) {
        elements.battAlert.value = "disable";
        elements.battAlertDiv.style.display = "none";
        elements.battAlert.disabled = true;
        window.bleAlertShown = true;
    } else if (!isBleEnabled) {
        elements.battAlert.disabled = true;
    }
}

function validateForm(elements, wifiMode) {
    const usesAP = wifiMode === "AP" || wifiMode === "APStation";
    const usesStation = wifiMode !== "AP";

    // Password validation
    if (usesAP) {
        const apPassLen = elements.apPassValue.value.length;
        if (apPassLen < 8 || apPassLen > 63) {
            return disableSubmitWithError("AP password length, min=8 max=63", 5000);
        }
        if (elements.apPassValue.value === "@meatpi#") {
            return disableSubmitWithError("AP password MUST be changed from default", 50000);
        }
    }

    if (usesStation) {
        const passLen = elements.passValue.value.length;
        if (passLen < 8 || passLen > 63) {
            return disableSubmitWithError("Station password length, min=8 max=63", 5000);
        }

        // SSID validation
        if (!validateLength(elements.ssidValue.value, 1, 32)) {
            return disableSubmitWithError("Station SSID length, min=1 max=32", 5000);
        }
    }

    
    // Port validation
    if (!validatePort(elements.tcpPortValue.value)) {
        return disableSubmitWithError("TCP Port value, min=1 max=65535", 5000);
    }
    if (!validatePort(elements.battAlertPort.value)) {
        return disableSubmitWithError("Battery Alert Port value, min=1 max=65535", 5000);
    }
    
    // BLE passkey validation - only validate if BLE is enabled
    const isBleEnabled = elements.bleStatus.value === "enable";
    if (isBleEnabled) {
        const blePass = elements.blePassValue.value;
        if (blePass.length !== 6 || blePass.charAt(0) === "0") {
            return disableSubmitWithError("BLE Passkey: 6 digits required, first digit cannot be 0", 5000);
        }

        if (blePass === "123456") {
            return disableSubmitWithError("BLE Passkey MUST be changed from default", 50000);
        }
    }
    
    // Sleep voltage validation
    const sleepVolt = parseFloat(elements.sleepVolt.value);
    if (sleepVolt < 12 || sleepVolt > 15) {
        return disableSubmitWithError("Sleep Voltage Value, min=12.0 max=15.0", 5000);
    }
    
    // Sleep disable agreement validation
    if (elements.sleepStatus.value === "disable" && elements.sleepDisableAgree.value === "no") {
        return disableSubmitWithError("You must agree to disable sleep mode", 5000);
    }
    
    return true;
}

function configureProtocolSettings(elements) {
    elements.tcpPortValue.disabled = false;
    elements.portType.selectedIndex = 0;
    elements.portType.disabled = false;
}

function configureSleepSettings(elements) {
    const sleepEnabled = elements.sleepStatus.value === "enable";
    const bleEnabled = elements.bleStatus.value === "enable";
    
    if (sleepEnabled) {
        if (!bleEnabled) {
            elements.battAlert.disabled = true;
        }
    } else {
        elements.battAlert.disabled = true;
        elements.battAlert.selectedIndex = 0;
    }
}

function configureMqttAndBatteryAlerts(elements) {
    // Battery alert div is always hidden in current logic
    elements.battAlertDiv.style.display = "none";
}

document.getElementById("defaultOpen").click();
function checkStatus() {
    const xhttp = new XMLHttpRequest();
    xhttp.onload = function() {
        var obj = JSON.parse(this.responseText);
        if(obj.wifi_mode == "APStation") {
            document.getElementById("wifi_mode_current").innerHTML = "AP+Station";
        } else if(obj.wifi_mode == "BLEStation") {
            document.getElementById("wifi_mode_current").innerHTML = "BLE+Station";
        } else if(obj.wifi_mode == "Station") {
            document.getElementById("wifi_mode_current").innerHTML = "Station";
        } else if(obj.wifi_mode == "AP") {
            document.getElementById("wifi_mode_current").innerHTML = "AP";
        } else if(obj.wifi_mode == "SmartConnect") {
            document.getElementById("wifi_mode_current").innerHTML = "SmartConnect";
        } else {
            document.getElementById("wifi_mode_current").innerHTML = obj.wifi_mode || "N/A";
        }

        document.getElementById("sta_status").innerHTML = obj.sta_status;
        document.getElementById("ap_channel_status").innerHTML = obj.ap_ch;
        document.getElementById("sta_ip").innerHTML = obj.sta_ip;

        const dnsMainEl = document.getElementById("dns_main_status");
        if (dnsMainEl) dnsMainEl.innerHTML = (obj.dns_main || "N/A");
        const dnsBackupEl = document.getElementById("dns_backup_status");
        if (dnsBackupEl) dnsBackupEl.innerHTML = (obj.dns_backup || "N/A");
        const timeSyncedEl = document.getElementById("time_synced_status");
        if (timeSyncedEl) timeSyncedEl.innerHTML = (obj.time_synced ? "Yes" : "No");

        document.getElementById("mdns").innerHTML = obj.mdns;
        document.getElementById("can_bitrate_status").innerHTML = obj.can_datarate;
        if(obj.can_mode == "normal") {
            document.getElementById("can_mode_status").innerHTML = "Normal";
        } else if(obj.can_mode == "silent") {
            document.getElementById("can_mode_status").innerHTML = "Silent";
        }
        if(obj.port_type == "tcp") {
            document.getElementById("port_type_status").innerHTML = "TCP";
        } else if(obj.port_type == "udp") {
            document.getElementById("port_type_status").innerHTML = "UDP";
        }
        document.getElementById("port_status").innerHTML = obj.port;
        // The git-describe tag IS the firmware version (issue #21): the old MeatPi
        // major.minor fw_version means nothing on this fork.
        document.getElementById("fw_version").innerHTML = obj.git_version;
        document.getElementById("hw_version").innerHTML = obj.hw_version;
        document.getElementById("protocol").value = obj.protocol;
        // poll_log is the native poller; fast_log records only CAN-filter/calculated
        // channels. auto_pid (the legacy ELM poller) is no longer listed: the config parser
        // coerces it to poll_log, so /check_status can never report it -- and if it ever did,
        // it deserves the warning rather than the silent pass it used to get.
        if (["poll_log", "fast_log"].indexOf(obj.protocol) === -1) {
            document.getElementById("autopid_warning_div").style.display = "block";
        }else {
            document.getElementById("autopid_warning_div").style.display = "none";
        }
        if(obj.subnet_overlap == "yes" && obj.ap_auto_disable != "enable") {
            document.getElementById("apconfig_warning_div").style.display = "block";
        } else {
            document.getElementById("apconfig_warning_div").style.display = "none";
        }
        document.getElementById("batt_voltage").innerHTML = obj.batt_voltage;
        if(document.getElementById("batt_alert").value == "enable") {
            document.getElementById("batt_alert_div").style.display = "none";
        } else if(document.getElementById("batt_alert").value == "disable") {
            document.getElementById("batt_alert_div").style.display = "none";
        }
        document.getElementById("obd_chip_status").innerHTML = obj.obd_chip_status || "N/A";
        document.getElementById("uptime").innerHTML = obj.uptime || "N/A";
        const restartLastResetEl = document.getElementById("restart_last_reset_reason");
        if (restartLastResetEl) restartLastResetEl.innerHTML = formatRestartTrackerValue(obj.restart_last_reset_reason);
        const restartLastPlannedEl = document.getElementById("restart_last_planned_reason");
        if (restartLastPlannedEl) restartLastPlannedEl.innerHTML = formatRestartTrackerValue(obj.restart_last_planned_reason);
        const restartLastSourceEl = document.getElementById("restart_last_source");
        if (restartLastSourceEl) restartLastSourceEl.innerHTML = formatRestartTrackerValue(obj.restart_last_source);
        const restartLastBootEl = document.getElementById("restart_last_boot_time");
        if (restartLastBootEl) restartLastBootEl.innerHTML = formatRestartTrackerLocalTime(obj.restart_last_boot_timestamp_unix);
        const restartBootCountEl = document.getElementById("restart_boot_count");
        if (restartBootCountEl) restartBootCountEl.innerHTML = (obj.restart_boot_count ?? 0);
        const restartUnexpectedCountEl = document.getElementById("restart_unexpected_reset_count");
        if (restartUnexpectedCountEl) restartUnexpectedCountEl.innerHTML = (obj.restart_unexpected_reset_count ?? 0);
        checkFirmwareUpdate();
    };
    xhttp.open("GET", "/check_status");
    xhttp.send();
}


function loadautoPID() {
    console.log("Loading auto PID data..."); 
    const xhttp = new XMLHttpRequest();
    xhttp.onload = function() {
        console.log("Server response:", this.responseText);
        if(this.responseText !== "NONE") {
            const data = JSON.parse(this.responseText);
            loadAutoTable(data);
            sensorsPageLoaded = true;   // the import passthrough may now trust the page
            document.getElementById("custom_pid_store").disabled = true;
        } else {
            console.log("No PID data found on server");
            togglePidPollingMinVoltageRow();
        }
    };
    xhttp.onerror = function(error) {
        console.error("Error loading auto PID:", error);
    };
    xhttp.open("GET", "/load_auto_pid");
    xhttp.send();
}

// Logger Settings (Task #5, trimmed in #5 datalogger-trim): one master "Logging"
// toggle drives the single real config key csv_log (a hidden select in the DOM).
function applyLoggerXor() {
    var masterEl = document.getElementById("logging_master");
    var cs = document.getElementById("csv_log");
    if (!masterEl || !cs) { return; }
    var csvShow = (masterEl.value === "enable");
    cs.value = csvShow ? "enable" : "disable";
    // Wide CSV (Task #11) controls, progressive: Polling Rate shown when CSV is active.
    // (Format toggle removed in Task #16 -- output is always Wide; the Polling Mode
    // dropdown removed in issue #53 -- the grid is always fixed-rate.)
    var hzRow = document.getElementById("csv_grid_hz_row");
    if (hzRow) hzRow.style.display = csvShow ? "" : "none";
    // Auto rate (issue #23): the manual Hz input is inert while the device tracks the
    // measured polling sweep, so grey it out.
    var hzAuto = document.getElementById("csv_grid_auto");
    var hzEl = document.getElementById("csv_grid_hz");
    if (hzAuto && hzEl) hzEl.disabled = hzAuto.checked;
    var reRow = document.getElementById("csv_require_engine_row");
    if (reRow) reRow.style.display = csvShow ? "" : "none";
}

async function postConfig() {
    var obj = {};
    document.getElementById("submit_button").disabled = true;
    await new Promise(resolve => setTimeout(resolve, 1000));
    const storeResult = await storeAutoTableData(true);   // skip the POST if the PID table is unchanged
    if (!storeResult) {
        document.getElementById("submit_button").disabled = false;
        return;
    }
    
    await new Promise(resolve => setTimeout(resolve, 1000));

    obj["wifi_mode"] = document.getElementById("wifi_mode").value;
    obj["ap_ch"] = document.getElementById("ap_ch_value").value;
    obj["ap_auto_disable"] = document.getElementById("ap_auto_disable").value;

    // Optional custom AP SSID
    {
        const enEl = document.getElementById("ap_ssid_enable");
        const valEl = document.getElementById("ap_ssid_value");
        const enabled = !!(enEl && enEl.checked);
        const ssid = (valEl && typeof valEl.value === 'string') ? valEl.value.trim() : "";

        if (enabled) {
            // Enforce min length client-side (firmware enforces too)
            if (ssid.length < 3 || ssid.length > 32) {
                showNotification("AP SSID must be 3–32 characters", "red");
                document.getElementById("submit_button").disabled = false;
                return;
            }
        }

        obj["ap_ssid_en"] = enabled ? "enable" : "disable";
        obj["ap_ssid"] = ssid;
    }
    obj["sta_ssid"] = document.getElementById("ssid_value").value;
    obj["sta_pass"] = document.getElementById("pass_value").value;
    obj["sta_security"] = document.getElementById("sta_security").value;
    // Keys with no UI (CAN, IMU, log period, SmartConnect leftovers): re-send the
    // stored values verbatim so they survive a Submit (see loadedPassthrough).
    Object.assign(obj, loadedPassthrough);
    obj["port_type"] = document.getElementById("port_type").value;
    obj["port"] = document.getElementById("tcp_port_value").value;
    obj["ap_pass"] = document.getElementById("ap_pass_value").value;
    obj["protocol"] = document.getElementById("protocol").value;
    obj["ble_pass"] = document.getElementById("ble_pass_value").value;
    obj["ble_status"] = document.getElementById("ble_status").value;
    obj["ble_power"] = document.getElementById("ble_power").value; // BLE TX power (dBm)
    obj["sleep_status"] = document.getElementById("sleep_status").value;
    obj["can_wake"] = document.getElementById("can_wake").value;
    obj["sleep_disable_agree"] = document.getElementById("sleep_disable_agree").value;
    obj["periodic_wakeup"] = document.getElementById("periodic_wakeup").value;
    obj["sleep_volt"] = document.getElementById("sleep_volt").value;
    obj["engine_volt"] = document.getElementById("engine_volt").value;
    obj["sleep_time"] = document.getElementById("sleep_time").value;
    obj["wakeup_interval"] = document.getElementById("wakeup_interval").value;
    obj["batt_alert"] = document.getElementById("batt_alert").value;
    obj["batt_alert_ssid"] = document.getElementById("batt_alert_ssid").value;
    obj["batt_alert_pass"] = document.getElementById("batt_alert_pass").value;
    obj["batt_alert_volt"] = document.getElementById("batt_alert_volt").value;
    obj["batt_alert_protocol"] = document.getElementById("batt_alert_protocol").value;
    let mqtt_txt = "mqtt://";
    let mqtt_url_val = mqtt_txt.concat(document.getElementById("batt_alert_url").value);
    obj["batt_alert_url"] = mqtt_url_val;
    obj["batt_alert_port"] = document.getElementById("batt_alert_port").value;
    obj["batt_alert_topic"] = document.getElementById("batt_alert_topic").value;
    obj["batt_alert_time"] = document.getElementById("batt_alert_time").value;
    obj["batt_mqtt_user"] = document.getElementById("batt_mqtt_user").value;
    obj["batt_mqtt_pass"] = document.getElementById("batt_mqtt_pass").value;
    applyLoggerXor();   // compose the real csv_log key from the master widget
    obj["csv_log"] = document.getElementById("csv_log").value;
    // Single-option selects removed from the UI (SD card / FATFS are the only
    // supported values) -- send the constants the firmware expects.
    obj["log_filesystem"] = "fatfs";
    obj["log_storage"] = "sdcard";
    // csv_grid_mode retired with the Event grid mode (issue #53) -- deliberately NOT sent,
    // so the key disappears from config.json on the first Submit after the update.
    // "auto" (issue #23): the grid tracks the measured polling sweep instead of a fixed Hz.
    obj["csv_grid_hz"] = document.getElementById("csv_grid_auto").checked
        ? "auto" : document.getElementById("csv_grid_hz").value;
    obj["csv_require_engine"] = document.getElementById("csv_require_engine").value;
    obj["led_blink"] = document.getElementById("led_blink").checked ? "enable" : "disable";

    // Collect fallback networks (max 5)
    try {
        const rows = document.querySelectorAll('#fallback_rows .fallback-row');
        const fallbacks = [];
        rows.forEach(r => {
            const ssid = r.querySelector('.fb-ssid').value.trim();
            const pass = r.querySelector('.fb-pass').value;
            const sec = r.querySelector('.fb-sec').value;
            if (ssid) {
                fallbacks.push({ ssid, pass, security: sec });
            }
        });
        obj["sta_fallbacks"] = fallbacks.slice(0, 5);
    } catch (e) {
        console.warn('fallback networks parse error', e);
        document.getElementById("submit_button").disabled = false;
    }

    var configJSON = JSON.stringify(obj, null, 0);
    
    // Send main configuration first
    const xhttp = new XMLHttpRequest();


    xhttp.open("POST", "/store_config");
    xhttp.setRequestHeader("Content-Type", "application/json");
    xhttp.onreadystatechange = function() {
        if (xhttp.readyState !== 4) return;
        if (xhttp.status >= 200 && xhttp.status < 300) {
            // Parse the honest envelope {reboot,applied,msg} (issue #39). Fall back to
            // legacy plain text (assume reboot) so UI and firmware can ship independently.
            let willReboot = true, msg = "";
            try {
                const r = JSON.parse(xhttp.responseText);
                willReboot = !!(r && r.reboot);
                if (r && typeof r.msg === "string") msg = r.msg;
            } catch (e) {
                willReboot = true;   // legacy firmware: plain-text "...Rebooting..."
            }
            if (willReboot) {
                // Reboot in progress: keep the reconnect UX (reload once it's back up).
                if (msg) showNotification(msg, "yellow", 9000);
                setTimeout(function() {
                    window.location.reload();
                }, 8000);
            } else {
                // Applied live: stay connected, no countdown; STEP 6 kept /load_config
                // fresh so the form is already correct. Re-enable Submit.
                showNotification(msg || "Configuration applied (no reboot).", "green", 6000);
                document.getElementById("submit_button").disabled = false;
            }
        } else {
            showNotification("Error saving configuration (HTTP " + xhttp.status + ")", "red");
            document.getElementById("submit_button").disabled = false;
        }
    };
    xhttp.send(configJSON);
}

function toggleApSsid() {
    const enEl = document.getElementById("ap_ssid_enable");
    const valEl = document.getElementById("ap_ssid_value");
    if (!enEl || !valEl) return;
    valEl.disabled = !enEl.checked;
    if (!enEl.checked) {
        // Keep value, but clear any browser validation UI
        try { valEl.setCustomValidity(""); } catch(_) {}
    } else {
        validateApSsid();
    }
}

function validateApSsid() {
    const enEl = document.getElementById("ap_ssid_enable");
    const valEl = document.getElementById("ap_ssid_value");
    if (!enEl || !valEl) return;
    if (!enEl.checked) {
        try { valEl.setCustomValidity(""); } catch(_) {}
        return;
    }
    const ssid = (valEl.value || "").trim();
    if (ssid.length < 3 || ssid.length > 32) {
        try { valEl.setCustomValidity("AP SSID must be 3–32 characters"); } catch(_) {}
    } else {
        try { valEl.setCustomValidity(""); } catch(_) {}
    }
}

function otaClick() {
    const fileInput = document.getElementById("ota_file");
    const submitButton = document.getElementById("ota_submit_button");
    const otaForm = document.getElementById("ota_form");

    const progressRow = document.getElementById("ota_progress_row");
    const progressFill = document.getElementById("ota_progress_fill");
    const progressText = document.getElementById("ota_progress_text");

    const setProgressVisible = (visible) => {
        if (progressRow) {
            progressRow.style.display = visible ? "" : "none";
        }
    };

    const setProgress = (percent, text) => {
        if (progressFill) {
            const clamped = Math.max(0, Math.min(100, Number(percent) || 0));
            progressFill.style.width = clamped + "%";
        }
        if (progressText) {
            progressText.textContent = text;
        }
    };

    const setProgressState = (state) => {
        if (!progressFill) return;
        progressFill.classList.remove("is-success", "is-error");
        if (state === "success") progressFill.classList.add("is-success");
        if (state === "error") progressFill.classList.add("is-error");
    };

    if (!fileInput || fileInput.files.length === 0) {
        showNotification("No files selected!", "red");
        alert("No files selected!");
        return;
    }

    if (!otaForm) {
        showNotification("OTA form not found", "red");
        if (submitButton) submitButton.disabled = false;
        return;
    }

    if (submitButton) submitButton.disabled = true;
    setProgressVisible(true);
    setProgressState("normal");
    setProgress(0, "Starting upload...");
    showNotification("Uploading firmware...", "green");

    const formData = new FormData(otaForm);
    const xhr = new XMLHttpRequest();
    const uploadUrl = otaForm.getAttribute("action") || "/upload/ota.bin";
    xhr.open("POST", uploadUrl);

    // Large uploads + slow links can take time; timeout mainly protects against a dead connection.
    xhr.timeout = 10 * 60 * 1000;

    let uploadCompleted = false;
    let totalBytes = 0;
    let loadedBytes = 0;
    let lastProgressAt = Date.now();
    let stallTimer = null;

    const cleanupTimers = () => {
        if (stallTimer) {
            clearInterval(stallTimer);
            stallTimer = null;
        }
    };

    const failAndUnlock = (message) => {
        cleanupTimers();
        setProgressState("error");
        showNotification(message, "red");
        // Keep whatever progress we have (helps indicate where it died).
        const percent = totalBytes > 0 ? Math.round((loadedBytes / totalBytes) * 100) : 0;
        setProgress(percent, message);
        if (submitButton) submitButton.disabled = false;
    };

    const startPostUploadWait = () => {
        cleanupTimers();
        setProgressState("success");
        setProgress(100, "Upload complete. Waiting 15 seconds...");
        showNotification("Upload complete. Waiting 15 seconds...", "green");

        let remaining = 15;
        const timer = setInterval(function () {
            remaining -= 1;
            if (remaining <= 0) {
                clearInterval(timer);
                setProgress(100, "Reconnecting...");
                // Device usually reboots after OTA; reload after delay.
                window.location.reload();
                return;
            }
            setProgress(100, `Upload complete. Waiting ${remaining} seconds...`);
        }, 1000);
    };

    xhr.upload.onprogress = function (event) {
        if (!event.lengthComputable) {
            setProgress(0, "Uploading...");
            return;
        }
        const percent = Math.round((event.loaded / event.total) * 100);
        totalBytes = event.total;
        loadedBytes = event.loaded;
        lastProgressAt = Date.now();
        if (percent >= 100 || (totalBytes > 0 && loadedBytes >= totalBytes)) uploadCompleted = true;
        setProgress(percent, `Upload: ${percent}%`);
    };

    xhr.upload.onload = function () {
        // Upload data fully handed off to the network stack/server.
        uploadCompleted = true;
        setProgress(100, "Upload sent. Finalizing...");
    };

    // If Wi-Fi drops mid-upload, browsers can sometimes hang without calling onerror immediately.
    // Light stall detection: if progress doesn't change for 15s during an active upload, warn/fail.
    stallTimer = setInterval(function () {
        if (uploadCompleted) return;
        if (totalBytes > 0 && loadedBytes > 0 && loadedBytes < totalBytes) {
            const stalledForMs = Date.now() - lastProgressAt;
            if (stalledForMs > 15000) {
                const offlineHint = (typeof navigator !== 'undefined' && navigator.onLine === false)
                    ? " (browser is offline)"
                    : "";
                try { xhr.abort(); } catch (e) { /* ignore */ }
                failAndUnlock("Update failed: connection lost during upload" + offlineHint + ". Reconnect and try again.");
            }
        }
    }, 1000);

    xhr.onerror = function () {
        // If the device reboots right after receiving the image, the browser may see a disconnect.
        if (uploadCompleted) {
            startPostUploadWait();
            return;
        }
        const offlineHint = (typeof navigator !== 'undefined' && navigator.onLine === false)
            ? " (browser is offline)"
            : "";
        failAndUnlock("Update failed: network error" + offlineHint + ". Reconnect and try again.");
    };

    xhr.onabort = function () {
        if (uploadCompleted) {
            startPostUploadWait();
            return;
        }
        failAndUnlock("Update aborted. If Wi-Fi dropped or the device was unplugged, reconnect and try again.");
    };

    xhr.ontimeout = function () {
        if (uploadCompleted) {
            startPostUploadWait();
            return;
        }
        failAndUnlock("Update timed out. Check Wi-Fi/device connection and try again.");
    };

    xhr.onreadystatechange = function () {
        if (xhr.readyState !== 4) return;

        if ((xhr.status >= 200 && xhr.status < 300) || (xhr.status === 0 && uploadCompleted)) {
            startPostUploadWait();
        } else {
            failAndUnlock(`Update failed (HTTP ${xhr.status}). Try again after reconnecting.`);
        }
    };

    xhr.send(formData);
}

function reboot() {
    const xhttp = new XMLHttpRequest();
    document.getElementById("reboot_button").disabled = true;
    showNotification("Rebooting please reconnect...", "yellow");
    xhttp.open("POST", "/system_reboot");
    xhttp.send("reboot");
}

function send_system_command(command) {
    const xhttp = new XMLHttpRequest();
    const data = {
        "command": command
    };
    xhttp.open("POST", "/system_commands");
    xhttp.send(JSON.stringify(data, null, 0));
}

async function downloadCfg() {
    const endpoints = [
        '/load_config',
        '/load_auto_pid'
    ];
    
    const delay = 500; 
    let combinedData = {};
    let hasErrors = false;
    
    try {
        for (let i = 0; i < endpoints.length; i++) {
            const endpoint = endpoints[i];
            
            try {
                const response = await fetch(endpoint);
                
                if (!response.ok) {
                    throw new Error(`HTTP error! status: ${response.status}`);
                }
                
                const data = await response.json();
                const key = endpoint.replace('/load_', '');
                combinedData[key] = data;
            } catch (fetchError) {
                hasErrors = true;
            }
            
            if (i < endpoints.length - 1) {
                await new Promise(resolve => setTimeout(resolve, delay));
            }
        }
        
        if (Object.keys(combinedData).length === 0) {
            throw new Error('No data was successfully fetched from any endpoint');
        }
        
        downloadTextFile(`config_${new Date().toISOString().split('T')[0]}.json`,
                         JSON.stringify(combinedData, null, 0));

        return true;
        
    } catch (error) {
        alert('Failed to download configuration');
        return false;
    }
}

async function uploadCfg() {
    const fileInput = document.getElementById('fileInput');
    const file = fileInput.files[0];
    if (!file) return;

    const endpointMap = {
        'config': '/store_config',
        'auto_pid': '/store_auto_data'
    };

    const delay = 200;

    try {
        const reader = new FileReader();
        
        reader.onload = async function(e) {
            try {
                // Reciprocal of the guard in applySensorsFile(). Checked on the RAW text and
                // before JSON.parse, because a sensor file is YAML: parsing it first would
                // fail with "Failed to parse configuration file" and never mention the tool
                // that does read it.
                if (/^\s*wican\s*:\s*"?sensors"?\s*$/m.test(e.target.result)) {
                    alert('That is a sensor file, not a full device backup. Load it from the Logger page with "Import Sensors".');
                    fileInput.value = '';
                    return;
                }
                const jsonData = JSON.parse(e.target.result);
                let hasErrors = false;

                for (const [key, endpoint] of Object.entries(endpointMap)) {
                    if (jsonData[key]) {
                        try {
                            const response = await fetch(endpoint, {
                                method: 'POST',
                                headers: {
                                    'Content-Type': 'application/json',
                                },
                                body: JSON.stringify(jsonData[key], null, 0)
                            });

                            if (!response.ok) {
                                hasErrors = true;
                                throw new Error(`HTTP error! status: ${response.status}`);
                            }

                            await new Promise(resolve => setTimeout(resolve, delay));

                        } catch (fetchError) {
                            hasErrors = true;
                        }
                    }
                }

                if (hasErrors) {
                    alert('Some configurations failed to upload');
                } else {
                    alert('Configuration uploaded successfully, Rebooting...');
                }
                
                fileInput.value = '';

            } catch (parseError) {
                alert('Failed to parse configuration file');
            }
        };

        reader.onerror = function() {
            alert('Error reading file');
        };

        reader.readAsText(file);

    } catch (error) {
        alert('Upload failed');
    }
}

// Config keys with no UI after the streamline: captured from /load_config in Load(),
// re-sent verbatim by postConfig() so config-file edits survive a Submit. The four
// pre-seeded defaults are mandatory keys -- /store_config rejects the whole POST when
// any of them is missing, so they must always be sent even if /load_config omits them.
// The home_*/drive_* SmartConnect keys are optional and captured only when present.
var loadedPassthrough = {
    can_datarate: "500K",   // NC platform is always 500K
    can_mode: "normal",
    imu_threshold: "8",     // IMU only feeds the removed SmartConnect logic
    log_period: "10",       // datalog period (no UI element after the trim)
};
// "debug" (#98): has no UI element, so without it here every Submit rewrote config.json without
// the key and silently turned debug logging back off. That was invisible while the flag only
// controlled serial output nobody can read on this device; it now also gates the event log's
// detail-only lines, so a Submit mid-debugging-session would quietly end the session.
var PASSTHROUGH_KEYS = ["can_datarate", "can_mode", "imu_threshold", "log_period", "debug",
    "home_ssid", "home_password", "home_security", "home_protocol",
    "drive_ssid", "drive_password", "drive_security", "drive_protocol",
    "drive_connection_type", "drive_mode_timeout"];
// Same idea for /store_auto_data (separate endpoint/lifecycle, captured in loadAutoTable):
var loadedStdPids = [];             // re-sent by storeAutoTableData
var loadedStandardPids = "disable"; // re-sent by storeAutoTableData
var loadedEcuProtocol = "6";        // re-sent by storeAutoTableData
var autoTableSavedJson = null;      // last committed/loaded auto_pid body; skip-unchanged baseline

async function Load() {
    const xhttp = new XMLHttpRequest();
xhttp.onload = async function() {
        var obj = JSON.parse(this.responseText);
        // Set WiFi mode by value (more robust than selectedIndex)
        const wifiModeEl = document.getElementById("wifi_mode");
        if (wifiModeEl) {
            const modeFromCfg = obj.wifi_mode || "AP";
            const hasOption = Array.from(wifiModeEl.options || []).some(o => o && o.value === modeFromCfg);
            if (hasOption) {
                wifiModeEl.value = modeFromCfg;
            }
        }

        // Capture every UI-less passthrough key for postConfig (see loadedPassthrough).
        PASSTHROUGH_KEYS.forEach(function(k) {
            if (obj[k] !== undefined && obj[k] !== null) loadedPassthrough[k] = obj[k];
        });

        
        if(obj.ap_auto_disable == "enable") {
            document.getElementById("ap_auto_disable").selectedIndex = "0";
        } else {
            document.getElementById("ap_auto_disable").selectedIndex = "1";
        }

        try { toggleApStationWarning(); } catch(_) {}

        // Custom AP SSID (optional, default disabled)
        {
            const enEl = document.getElementById("ap_ssid_enable");
            const valEl = document.getElementById("ap_ssid_value");
            if (enEl) {
                enEl.checked = (obj.ap_ssid_en === "enable");
            }
            if (valEl) {
                valEl.value = obj.ap_ssid || "";
            }
            try { toggleApSsid(); } catch(_) {}
        }

        var ch = parseInt(obj.ap_ch);
        ch = ch - 1;
        document.getElementById("ap_ch_value").selectedIndex = ch.toString();
        document.getElementById("ssid_value").value = obj.sta_ssid;
        document.getElementById("pass_value").value = obj.sta_pass;
        document.getElementById("sta_security").value = obj.sta_security || "wpa3";			
        if(obj.port_type == "tcp") {
            document.getElementById("port_type").selectedIndex = "0";
        } else if(obj.port_type == "udp") {
            document.getElementById("port_type").selectedIndex = "1";
        }
        if(obj.ble_status == "enable") {
            document.getElementById("ble_status").selectedIndex = 0;
        } else if(obj.ble_status == "disable") {
            document.getElementById("ble_status").selectedIndex = 1;
        }
        if(obj.sleep_status == "enable") {
            document.getElementById("sleep_status").selectedIndex = "0";
        } else if(obj.sleep_status == "disable") {
            document.getElementById("sleep_status").selectedIndex = "1";
        }

        // Wake on CAN (issue #4). Anything that is not an explicit "disable" shows as Enable,
        // matching config_server_get_can_wake() exactly: a device provisioned before this key
        // existed has no can_wake field at all, and it must display as ON rather than silently
        // reading back Disable and then writing that back on the next save.
        document.getElementById("can_wake").selectedIndex = (obj.can_wake == "disable") ? 1 : 0;

        if(obj.sleep_disable_agree == "yes") {
            document.getElementById("sleep_disable_agree").selectedIndex = "1";
        } else {
            document.getElementById("sleep_disable_agree").selectedIndex = "0";
        }
        toggleSleepWarning();
        // Pinned, not mirrored: periodic wakeup is retired and the firmware force-disables
        // the key in the config parser. A legacy config.json may still say "enable" (
        // /load_config streams the raw file), so ignore it and post "disable" back.
        document.getElementById("periodic_wakeup").value = "disable";

        document.getElementById("batt_mqtt_user").value = obj.batt_mqtt_user;
        document.getElementById("batt_mqtt_pass").value = obj.batt_mqtt_pass;
        if(obj.batt_alert_time == "1") {
            document.getElementById("batt_alert_time").selectedIndex = "0";
        } else if(obj.batt_alert_time == "6") {
            document.getElementById("batt_alert_time").selectedIndex = "1";
        } else if(obj.batt_alert_time == "12") {
            document.getElementById("batt_alert_time").selectedIndex = "2";
        } else if(obj.batt_alert_time == "24") {
            document.getElementById("batt_alert_time").selectedIndex = "3";
        }
        if(document.getElementById("batt_alert").value == "enable") {
            document.getElementById("batt_alert_div").style.display = "none";
        } else if(document.getElementById("batt_alert").value == "disable") {
            document.getElementById("batt_alert_div").style.display = "none";
        }

        // --- Restored settings population (regression fix: commit d372fc9 over-cut this block,
        //     causing every Submit to persist stock HTML defaults). MQTT-gateway lines intentionally
        //     omitted (feature removed by the trim); protocol is populated by checkStatus(). ---
        // Datalogger master + wide-CSV grid controls (firmware default is 10 Hz; the grid
        // is always fixed-rate -- csv_grid_mode retired in issue #53, ignored if present).
        var _cs_on = (obj.csv_log === "enable");
        document.getElementById("csv_log").value = _cs_on ? "enable" : "disable";
        document.getElementById("logging_master").value = _cs_on ? "enable" : "disable";
        document.getElementById("csv_grid_auto").checked = (obj.csv_grid_hz === "auto");
        var _hz = parseInt(obj.csv_grid_hz, 10);   // NaN when "auto" -> input keeps the 10 default
        document.getElementById("csv_grid_hz").value = (_hz >= 1 && _hz <= WICAN_LOG_MAX_HZ) ? _hz : 10;
        document.getElementById("csv_require_engine").value = (obj.csv_require_engine === "disable") ? "disable" : "enable";
        applyLoggerXor();

        // Activity-LED blink toggle: default ON when the key is absent (old config).
        document.getElementById("led_blink").checked = (obj.led_blink !== "disable");

        const blePowerVal = ("ble_power" in obj) ? obj.ble_power : 9;
        document.getElementById("ble_power").value = blePowerVal;
        document.getElementById("ble_power_value").textContent = blePowerVal;

        document.getElementById("tcp_port_value").value = obj.port;
        document.getElementById("ap_pass_value").value = obj.ap_pass;
        document.getElementById("ble_pass_value").value = obj.ble_pass;
        document.getElementById("sleep_volt").value = obj.sleep_volt;
        document.getElementById("sleep_volt_value").textContent = obj.sleep_volt;
        if (obj.engine_volt !== undefined) {   // null-guard for old configs missing the key
            document.getElementById("engine_volt").value = obj.engine_volt;
            document.getElementById("engine_volt_value").textContent = obj.engine_volt;
        }
        document.getElementById("sleep_time").value = obj.sleep_time;
        document.getElementById('sleep_time_value').textContent = obj.sleep_time;
        document.getElementById("wakeup_interval").value = obj.wakeup_interval;
        document.getElementById('wakeup_interval_value').textContent = obj.wakeup_interval;
        document.getElementById("batt_alert").value = "disable";
        document.getElementById("batt_alert_ssid").value = obj.batt_alert_ssid;
        document.getElementById("batt_alert_pass").value = obj.batt_alert_pass;
        document.getElementById("batt_alert_volt").value = obj.batt_alert_volt;
        document.getElementById("batt_alert_protocol").value = obj.batt_alert_protocol;
        document.getElementById("batt_alert_url").value = obj.batt_alert_url.slice(7);
        document.getElementById("batt_alert_port").value = obj.batt_alert_port;
        document.getElementById("batt_alert_topic").value = obj.batt_alert_topic;
        loadautoPID();

        // Load fallback networks if present
        try {
            const fb = Array.isArray(obj.sta_fallbacks) ? obj.sta_fallbacks : [];
            renderFallbackNetworks(fb);
        } catch(e) {
            renderFallbackNetworks([]);
        }

        // Apply mode-dependent enable/disable rules after values are loaded
        try { toggleApStationWarning(); } catch(_) {}
        try { submit_enable(); } catch(_) {}

        document.querySelector(".store").disabled = true;
        document.getElementById("submit_button").disabled = true;
    };
    checkStatus();
    xhttp.open("GET", "/load_config");
    xhttp.send();


    // Initialize AP+Station warning visibility
    try { toggleApStationWarning(); } catch(_) {}

    // Initialize the logger low-voltage defaults before auto_pid.json is loaded.
    try { togglePidPollingMinVoltageRow(); } catch(_) {}

    // Initialize AP SSID input state
    try { toggleApSsid(); } catch(_) {}

    // Sleep-countdown banner (#85). Started once here and never stopped: it must keep watching
    // whichever tab the user is on, because the warning matters most away from the Console.
    try { sleep_status_poll_start(); } catch(_) {}

    // Initialize lucide icons
    if (typeof lucide !== 'undefined' && lucide.createIcons) {
        lucide.createIcons();
    }
}

// ---- CSV datalogger live status poll (renders the Console recorder card) ----
// The Console's Start Trip button is the only start/stop UI (issue #21 removed the
// Logger page's Start button). Firmware status is the source of truth: on every poll
// we reconcile from /csv_status (manual_override || session_active), robust to
// cross-core latency and to start/stop happening from another client or from ignition.
function csv_notify(m, c) {
    if (typeof showNotification === 'function') showNotification(m, c); else console.log(m);
}
function csv_status_render(j) {
    var on = !!(j && (j.manual_mode === 'on' || j.session_active));
    console_status_render(j, on);
}
function csv_status_tick() {
    if (window._csvStatusInFlight) return;
    window._csvStatusInFlight = true;
    fetch('/csv_status').then(function(r) { return r.json(); })
        .then(function(j) { csv_status_render(j); })
        .catch(function() {})
        .finally(function() { window._csvStatusInFlight = false; });
}
function csv_status_poll_start() {
    if (window._csvStatusTimer) return;
    csv_status_tick();
    window._csvStatusTimer = setInterval(csv_status_tick, 1500);
}
function csv_status_poll_stop() {
    if (window._csvStatusTimer) { clearInterval(window._csvStatusTimer); window._csvStatusTimer = null; }
}

// ---- Sleep-countdown banner (issue #85) ------------------------------------------------------
// Warns that the device has started counting down to sleep, and shows how long is left. When the
// countdown ends the device powers down its radio and drops off the network, so someone could be
// seconds from losing the connection mid trip-download or mid config-change with no warning.
//
// Deliberately NOT tied to the Console tab: the csv_status poll stops on every tab switch, and the
// people who most need this warning are the ones on the Settings tab. This poll runs everywhere,
// for the whole life of the page.
//
// It also does NOT reuse showNotification(): that is a single-slot toast with one global auto-hide
// timer, so a banner redrawing itself every second would wipe out every "settings saved" message,
// and every message would wipe out the banner. #sleep_banner is its own element in the same stack.
// Two cadences (see sleep_status_cadence). Idle is the steady state and is where ~all of the
// requests go, so it is the slow one; the fast one only runs while a countdown is actually live,
// where it has to be ~2 s to catch the brief window between "sleeping" and the radio going down.
var SLEEP_POLL_IDLE_MS = 5000;
var SLEEP_POLL_FAST_MS = 2000;
// Don't show the banner until the countdown has been running this long. Every boot starts a
// countdown ~1.2 s in and cancels it ~2 s later once the ECU is detected; without this the banner
// would flash on screen on every single boot. Measured as (secs_total - secs_left) rather than by
// counting in the browser, so a page opened in the MIDDLE of a real countdown still shows the
// banner on its very first poll instead of waiting 5 s to catch up.
// KEEP IN STEP with SLEEP_COUNTDOWN_REAL_MS in main/sleep_mode.c -- the same threshold in two
// languages, so a countdown too short to be worth a log line is also too short to warn anyone about.
var SLEEP_BANNER_MIN_ELAPSED_S = 5;
// Consecutive failed polls before we conclude the device slept. Three at 2 s covers a reload or a
// brief WiFi hiccup without crying wolf.
var SLEEP_LOST_POLLS = 3;

function sleep_banner_el() { return document.getElementById('sleep_banner'); }

// _sleepDeadline is the single "is a countdown being shown" flag: non-null means the 1 Hz renderer
// owns the banner text, null means it must not touch it. A separate boolean would be a second thing
// to keep in step, and if the two ever disagreed the renderer would either freeze the countdown or
// overwrite a terminal message.
function sleep_banner_hide() {
    var el = sleep_banner_el();
    if (el) { el.classList.remove('show'); el.classList.remove('lost'); }
    window._sleepDeadline = null;
}

function sleep_banner_mmss(secs) {
    if (secs < 0) secs = 0;
    var m = Math.floor(secs / 60);
    var s = secs % 60;
    return m + ':' + (s < 10 ? '0' + s : String(s));
}

// Redraws once a second off a local deadline, so the number ticks smoothly between polls.
// Cheap-exit first: with no countdown showing this must not even touch the DOM, because it runs
// every second for the whole life of the page.
function sleep_banner_render() {
    if (window._sleepDeadline == null) return;
    var el = sleep_banner_el();
    if (!el) return;
    var left = Math.max(0, Math.round((window._sleepDeadline - Date.now()) / 1000));
    var volts = (typeof window._sleepVolts === 'number' && window._sleepVolts > 0)
              ? (' — battery low (' + window._sleepVolts.toFixed(2) + ' V).')
              : '.';
    // "Ignition on", not "start the car" (#98): what actually cancels the countdown is the ECU
    // answering, and it answers at key-on with the engine not turning. Telling someone to start
    // the engine would repeat the very naming mistake #98 set out to fix.
    el.textContent = 'Device will sleep in ' + sleep_banner_mmss(left) + volts +
                     ' Turn the ignition on to cancel it.';
}

// Terminal state: the device is going, or already gone. Freeze the clock (null deadline) so the
// 1 Hz renderer stops overwriting the message, and colour it as the bad case.
function sleep_banner_final(msg) {
    var el = sleep_banner_el();
    if (!el) return;
    el.classList.add('lost');
    el.classList.add('show');
    el.textContent = msg;
    window._sleepDeadline = null;
}

// Re-arm the poll at the cadence the current state deserves. Idle needs no urgency (the banner
// cannot appear until 5 s of countdown has passed anyway, and a countdown lasts minutes), but once
// one is running we need the fast cadence: the firmware stays reachable for only ~2 s after it
// publishes "sleeping", and missing that window costs the goodbye message.
function sleep_status_cadence(ms) {
    if (window._sleepPollMs === ms) return;
    window._sleepPollMs = ms;
    if (window._sleepStatusTimer) clearInterval(window._sleepStatusTimer);
    window._sleepStatusTimer = setInterval(sleep_status_tick, ms);
}

function sleep_status_tick() {
    if (window._sleepStatusInFlight) return;
    window._sleepStatusInFlight = true;
    fetch('/sleep_status').then(function(r) { return r.json(); }).then(function(j) {
        window._sleepFails = 0;
        var el = sleep_banner_el();
        if (!el) return;
        var counting = !!(j && j.state === 'countdown');
        sleep_status_cadence(counting ? SLEEP_POLL_FAST_MS : SLEEP_POLL_IDLE_MS);

        // The teardown has begun and the network is about to go. Say goodbye while we still can.
        // MUST come before the hide branch: the firmware publishes "sleeping" at the START of its
        // teardown and stays reachable ~2 s longer, so a poll usually lands here. Falling through to
        // hide() would clear the deadline and the connection-lost path would then never fire -- the
        // banner would just vanish at about 0:03.
        if (j && j.state === 'sleeping') {
            sleep_banner_final('Device is going to sleep now and will drop off the network. ' +
                               'It wakes when the ignition comes on.');
            return;
        }

        el.classList.remove('lost');

        if (!counting) {
            // Includes "off" (sleep disabled), "normal", and "waking" (coming back, so no warning
            // is wanted). The grace delay applies only to SHOWING the banner -- a cancelled
            // countdown clears it immediately.
            sleep_banner_hide();
            return;
        }

        var left = Number(j.secs_left) || 0;
        var total = Number(j.secs_total) || 0;
        if (Math.max(0, total - left) < SLEEP_BANNER_MIN_ELAPSED_S) {
            sleep_banner_hide();
            return;
        }

        // Re-anchor every poll. This is also what makes a countdown that JUMPS BACK UP correct for
        // free: leaving the low-voltage state abandons the countdown and re-entering re-arms the
        // full time, so we just render whatever the device now says.
        window._sleepVolts = Number(j.voltage) || 0;
        window._sleepDeadline = Date.now() + left * 1000;
        el.classList.add('show');
        sleep_banner_render();
    }).catch(function() {
        window._sleepFails = (window._sleepFails || 0) + 1;
        // Only a banner that was already counting may turn into "probably slept". Otherwise a reboot
        // or a dropped WiFi link would conjure a sleep warning out of nothing.
        if (window._sleepDeadline != null && window._sleepFails >= SLEEP_LOST_POLLS) {
            sleep_banner_final('Connection lost — the device has probably gone to sleep. ' +
                               'It wakes when the car wakes.');
        }
    }).finally(function() { window._sleepStatusInFlight = false; });
}

// Started once from Load() and deliberately NEVER stopped -- unlike csv_status_poll_stop(), there is
// no matching stop, because the whole point is to warn someone who is on a different tab.
function sleep_status_poll_start() {
    if (window._sleepStatusTimer) return;
    window._sleepFails = 0;
    sleep_status_tick();
    sleep_status_cadence(SLEEP_POLL_IDLE_MS);
    setInterval(sleep_banner_render, 1000);   // handle not kept: nothing ever cancels it
}
function isNameUnique(name) {
    return canData.every((item) => item["Name"] !== name);
}

function toggleSleepWarning() {
    const sleepStatus = document.getElementById("sleep_status").value;
    const sleepWarningDiv = document.getElementById("sleep_warning_div");
    const agreementSelect = document.getElementById("sleep_disable_agree");
    
    if (sleepStatus === "disable") {
        sleepWarningDiv.style.display = "block";
        agreementSelect.value = "no";
    } else {
        sleepWarningDiv.style.display = "none";
    }
}

async function scanWifiNetworks() {
    const scanButton = document.getElementById('wifi_scan_button');
    const networksList = document.getElementById('wifi_networks_list');
    const networksRow = document.getElementById('wifi_networks_row');
    
    try {
        scanButton.disabled = true;
        scanButton.textContent = "Scanning...";
        
        const response = await fetch('/wifi_scan');
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        
        const data = await response.text();

        // Clear existing options
        networksList.innerHTML = '<option value="">Select a network...</option>';

        if (data && data !== "NONE") {
            try {
                const scanResult = JSON.parse(data);

                // Check for error response from server
                if (scanResult.error) {
                    throw new Error(scanResult.error);
                }

                if (scanResult.networks && Array.isArray(scanResult.networks)) {
                    // Filter out networks with empty SSID and sort by signal strength
                    const validNetworks = scanResult.networks
                        .filter(network => network.ssid && network.ssid.trim() !== '')
                        .sort((a, b) => b.rssi - a.rssi); // Sort by signal strength (strongest first)
                    
                    // Remove duplicates (same SSID, keep the strongest signal)
                    const uniqueNetworks = [];
                    const seenSSIDs = new Set();
                    
                    validNetworks.forEach(network => {
                        if (!seenSSIDs.has(network.ssid)) {
                            seenSSIDs.add(network.ssid);
                            uniqueNetworks.push(network);
                        }
                    });
                    
                    if (uniqueNetworks.length > 0) {
                        uniqueNetworks.forEach(network => {
                            const option = document.createElement('option');
                            option.value = network.ssid;
                            
                            // Create a nice display name with signal strength and security
                            const signalBars = getSignalQuality(network.rssi);
                            const security = getSecurityType(network.auth_mode);
                            option.textContent = `${network.ssid} (${signalBars}, ${network.rssi} dBm, ${security})`;
                            
                            networksList.appendChild(option);
                        });
                        networksRow.style.display = 'table-row';
                        showNotification(`Found ${uniqueNetworks.length} WiFi networks`, "green");
                    } else {
                        showNotification("No valid networks found", "yellow");
                    }
                } else {
                    throw new Error("Invalid scan data format");
                }
            } catch (parseError) {
                console.error('Parse error:', parseError);
                showNotification("WiFi Scan failed " + parseError, "red");
            }
        } else {
            showNotification("No networks found", "yellow");
        }
    } catch (error) {
        console.error('WiFi scan error:', error);
        showNotification("WiFi scan failed: " + error.message, "red");
    } finally {
        scanButton.disabled = false;
        scanButton.textContent = "Scan";
    }
}

function getSignalQuality(rssi) {
    // Convert RSSI to signal quality text
    if (rssi >= -50) return "Excellent"; // Excellent
    if (rssi >= -60) return "Good";      // Good  
    if (rssi >= -70) return "Fair";      // Fair
    return "Weak";                       // Weak
}

function getSecurityType(authMode) {
    // Simplify auth mode display
    switch(authMode) {
        case "OPEN": return "Open";
        case "WPA_PSK": return "WPA";
        case "WPA2_PSK": return "WPA2";
        case "WPA_WPA2_PSK": return "WPA/WPA2";
        case "WPA2_WPA3_PSK": return "WPA2/WPA3";
        case "WPA3_PSK": return "WPA3";
        default: return authMode;
    }
}

function selectWifiNetwork() {
    const networksList = document.getElementById('wifi_networks_list');
    const ssidInput = document.getElementById('ssid_value');
    
    if (networksList.value) {
        ssidInput.value = networksList.value;
        submit_enable(); // Trigger form validation
        enableAutoStoreButton(); // Enable store button if it exists
    }
}


// ---- Field Console (issue #5): one-tap trip landing surface ----
function console_status_render(j, on) {
    var dot = document.getElementById('console_rec_dot');
    var state = document.getElementById('console_rec_state');
    var file = document.getElementById('console_rec_file');
    var btn = document.getElementById('console_rec_btn');
    if (!dot || !state || !btn) return;
    var live = !!(j && j.session_active);
    var armed = !!(j && j.manual_mode === 'on' && !live);
    dot.className = 'rec-dot' + (live ? ' live' : (armed ? ' armed' : ''));
    state.textContent = live ? 'Recording' : (armed ? 'Armed' : 'Idle');
    if (file) file.textContent = live ? (j.file || '') : (armed ? 'waiting for data\u2026' : '\u00a0');
    btn.textContent = on ? 'Stop Trip' : 'Start Trip';
    btn.className = 'console-rec-btn' + (on ? ' stop' : '');
    var markBtn = document.getElementById('console_mark_btn');
    if (markBtn) markBtn.style.display = live ? '' : 'none';   // only offer Mark when a file is actually recording
    var rows = document.getElementById('console_rec_rows');
    var dropped = document.getElementById('console_rec_dropped');
    var cols = document.getElementById('console_rec_cols');
    if (rows) rows.textContent = (j && j.rows_written) || 0;
    if (dropped) dropped.textContent = (j && j.rows_dropped) || 0;
    if (cols) cols.textContent = (j && j.columns) || 0;
    var sdDot = document.getElementById('console_dot_sd');
    var sdChip = document.getElementById('console_chip_sd');
    if (sdDot && j && typeof j.sd_mounted === 'boolean') {
        sdDot.className = 'chip-dot ' + (j.sd_mounted ? 'ok' : 'bad');
        if (sdChip) sdChip.textContent = j.sd_mounted ? 'mounted' : 'missing';
    }
    consoleEventsTick();   // piggyback the event-card refresh on the csv_status poll (5s-throttled, tab-gated)
}

function consoleRecClick() {
    var btn = document.getElementById('console_rec_btn');
    if (!btn) return;
    var op = btn.classList.contains('stop') ? 'stop' : 'start';
    fetch('/csv_logger?op=' + op, { method: 'POST' })
        .then(function(r) { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
        .then(function(j) {
            csv_status_render(j);
            if (op === 'start') {
                csv_notify(j.session_active ? 'Trip recording started' : 'Trip armed \u2014 waiting for data',
                           j.session_active ? 'green' : 'orange');
            } else {
                csv_notify('Trip stopped', 'blue');
                setTimeout(consoleLoadTrips, 800);
            }
            csv_status_poll_start();
        })
        .catch(function(e) { csv_notify('Trip control failed: ' + e.message, 'red'); });
}

function consoleMarkClick() {
    var btn = document.getElementById('console_mark_btn');
    if (btn && btn.disabled) return;                 // debounce (also coalesces the one-shot)
    fetch('/csv_logger?op=mark', { method: 'POST' })
        .then(function(r) {
            if (r.status === 409) throw new Error('no trip is recording');
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.json();
        })
        .then(function(j) {
            csv_status_render(j);                    // reply is the live status JSON, like start/stop
            if (btn) {
                btn.disabled = true;
                btn.textContent = 'Marked ✓';
                setTimeout(function() { btn.textContent = 'Mark Event'; btn.disabled = false; }, 1200);
            }
        })
        .catch(function(e) { csv_notify('Mark failed: ' + e.message, 'red'); });
}

function consoleFmtSize(b) {
    b = Number(b) || 0;
    if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB';
    if (b >= 1024) return (b / 1024).toFixed(0) + ' KB';
    return b + ' B';
}

function consoleLoadTrips() {
    fetch('/csv_list')
        .then(function(r) { return r.json(); })
        .then(function(j) {
            var box = document.getElementById('console_trips');
            if (!box) return;
            var files = (j && Array.isArray(j.files)) ? j.files.slice(0, 12) : [];
            if (!files.length) {
                box.innerHTML = '<div class="console-empty">No trips yet \u2014 press Start Trip to record one.</div>';
                return;
            }
            box.innerHTML = files.map(function(f) {
                var name = String(f.name || '');
                var meta = consoleFmtSize(f.size) + (f.mtime ? ' \u2022 ' + filesFmtDate(f.mtime) : '');
                return '<div class="console-trip"><div><div class="t-name">' + name + '</div>' +
                       '<div class="t-meta">' + meta + '</div></div>' +
                       '<a class="t-dl" href="/download_csv?file=' + encodeURIComponent(name) + '" download>' +
                       '<button class="console-mini-btn" type="button">Download</button></a></div>';
            }).join('');
        })
        .catch(function() {});
}

function consoleLoadChips() {
    fetch('/check_status')
        .then(function(r) { return r.json(); })
        .then(function(d) {
            var wifiDot = document.getElementById('console_dot_wifi');
            var wifiChip = document.getElementById('console_chip_wifi');
            var proto = document.getElementById('console_chip_proto');
            var fw = document.getElementById('console_chip_fw');
            var staUp = (d && d.sta_status === 'Connected');
            if (wifiDot) wifiDot.className = 'chip-dot ' + (staUp ? 'ok' : 'bad');
            if (wifiChip) wifiChip.textContent = staUp ? (d.sta_ip || 'connected') : 'AP only';
            // Friendly mode labels; keep the raw protocol string in the tooltip
            // (runbooks reference the raw names) and fall back to it for unknowns.
            // Compact chip labels; match the leading words of the protocol <select> options
            // (homepage_full.html ~1495) so the two stay a single friendly-name source.
            var MODE_NAMES = {poll_log:'Datalogger', fast_log:'Passive Logger', elm327:'OBD App', auto_pid:'Legacy AutoPID', slcan:'Bench SLCAN'};
            if (proto) {
                var p = (d && d.protocol) || '';
                proto.textContent = MODE_NAMES[p] || p || '\u2013';
                proto.title = p;
            }
            if (fw) fw.textContent = (d && (d.git_version || d.fw_version)) || '\u2013';
        })
        .catch(function() {});
}

function consoleRefresh() {
    consoleLoadTrips();
    consoleLoadChips();
    window._consoleEvtLast = Date.now();
    consoleLoadEvents();
}

function consoleEvtSeverity(code) {
    if (code.indexOf('FAIL') !== -1) return 'er';
    if (code === 'REAPER_RESUME' || code === 'IGNITION_OFF' || code === 'WARN' ||
        code === 'OTA_START' || code === 'DATALOG_PARK') return 'wn';
    return 'ok';
}

function consoleFmtUptime(ms) {
    var s = Math.floor(ms / 1000);
    if (s < 60) return s + 's';
    var m = Math.floor(s / 60); s %= 60;
    if (m < 60) return m + 'm' + (s < 10 ? '0' : '') + s + 's';
    var h = Math.floor(m / 60); m %= 60;
    return h + 'h' + (m < 10 ? '0' : '') + m + 'm';
}

// Refresh the event card on the shared 1.5s csv_status cadence, throttled to >=5s
// and only while the Console tab is actually visible (the logger tab runs the same poll).
function consoleEventsTick() {
    var tab = document.getElementById('console_tab');
    if (!tab || tab.style.display !== 'block') return;
    var now = Date.now();
    if (window._consoleEvtLast && (now - window._consoleEvtLast) < 5000) return;
    window._consoleEvtLast = now;
    consoleLoadEvents();
}

function consoleLoadEvents() {
    fetch('/event_log/ram')
        .then(function(r) { return r.text(); })
        .then(function(txt) {
            var box = document.getElementById('console_events');
            if (!box) return;
            // The RAM event ring rarely changes between 5s ticks; skip the ~75-node rebuild
            // when the raw payload is byte-identical to the last render.
            if (txt === window._consoleEvtRaw) return;
            window._consoleEvtRaw = txt;
            var lines = txt.split('\n').filter(function(s) { return s.trim().length > 0; });
            lines.reverse();                 // /event_log/ram streams oldest->newest
            lines = lines.slice(0, 15);      // newest-first, cap 15
            if (!lines.length) {
                box.innerHTML = '<div class="console-empty">No events yet</div>';
                return;
            }
            // Grammar (event_log.c:148): "<ts> up=<ms>ms <CODE %-12s> <detail>"
            // where <ts> is "YYYY-MM-DD HH:MM:SS" (synced) or the literal "unsynced".
            var re = /^(?:(\d{4})-\d{2}-\d{2} (\d{2}:\d{2}:\d{2})|unsynced) up=(\d+)ms (\S+)\s*(.*)$/;
            box.textContent = '';
            lines.forEach(function(line) {
                var m = re.exec(line);
                var t, code, detail;
                if (m) {
                    var synced = m[2] && parseInt(m[1], 10) >= 2000; // pre-2000/unsynced -> uptime
                    t = synced ? m[2] : ('up ' + consoleFmtUptime(parseInt(m[3], 10)));
                    code = m[4];
                    detail = m[5];
                } else {
                    t = ''; code = 'EVENT'; detail = line;
                }
                var row = document.createElement('div');
                row.className = 'console-evt';
                var tEl = document.createElement('span');
                tEl.className = 'e-t';
                tEl.textContent = t;
                tEl.title = line;            // full raw line (date + uptime) on hover
                var tag = document.createElement('span');
                tag.className = 'console-tag t-' + consoleEvtSeverity(code);
                tag.textContent = code;
                var d = document.createElement('span');
                d.className = 'e-d';
                d.textContent = detail;      // textContent everywhere: no HTML injection
                row.appendChild(tEl); row.appendChild(tag); row.appendChild(d);
                box.appendChild(row);
            });
        })
        .catch(function() {});               // silent, same as consoleLoadTrips
}

document.getElementById("defaultOpen").click();
