// Pure-logic tests for the standalone Wi-Fi diagnostic page (issue #105).
//
// main/web/wifi_diag.html is embedded verbatim and is NOT covered by lint_web.py or build_web.py --
// both of those own only the single-page app. So the only automated guard this page gets is here.
//
// The formatters are small, but the RSSI thresholds are not just cosmetic: the page colours a value
// red/amber/green using the same boundaries the firmware uses for its histogram buckets and its
// findings. If those two drift apart, the page calls a signal "good" in green while the report
// generated from the same device calls it weak -- a contradiction that would send someone chasing
// the wrong fault, and one no reviewer would catch by eye across a .c and an .html file. The last
// test pins them together.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import vm from 'node:vm';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const PAGE = join(REPO, 'main', 'web', 'wifi_diag.html');
const IMPL = join(REPO, 'components', 'wifi_diag', 'wifi_diag.c');

function stubElement() {
    const el = {
        style: {}, dataset: {}, className: '', textContent: '', innerHTML: '',
        checked: false, disabled: false, firstChild: null,
        classList: { add() {}, remove() {}, toggle() {}, contains: () => false },
        appendChild() {}, removeChild() {}, remove() {},
        addEventListener() {}, removeEventListener() {},
        setAttribute() {}, getAttribute: () => null,
        querySelector: () => null, querySelectorAll: () => [],
    };
    return el;
}

// Evaluate the page's inline <script> with just enough DOM to survive load. The script calls load()
// on the last line; the stub fetch rejects, which the page's own .catch() handles (that path is the
// "device unreachable" banner, so exercising it here is a feature, not an accident).
function loadPage() {
    const html = readFileSync(PAGE, 'utf8');
    const m = html.match(/<script>([\s\S]*?)<\/script>/);
    assert.ok(m, 'wifi_diag.html no longer contains an inline <script> block');

    const elCache = new Map();
    const ctx = {
        console,
        document: {
            getElementById(id) {
                if (!elCache.has(id)) elCache.set(id, stubElement());
                return elCache.get(id);
            },
            createElement: () => stubElement(),
            createElementNS: () => stubElement(),
            createTextNode: () => ({}),
            addEventListener() {},
        },
        fetch: async () => { throw new Error('sandbox: no network'); },
        setTimeout, clearTimeout, setInterval, clearInterval,
        Date,
    };
    ctx.window = ctx;
    ctx.globalThis = ctx;
    vm.createContext(ctx);
    vm.runInContext(m[1], ctx, { filename: 'wifi_diag.html' });
    return ctx;
}

test('page script loads and exposes its formatters', () => {
    const ctx = loadPage();
    for (const name of ['secs', 'bytes', 'rssiClass', 'render', 'load']) {
        assert.equal(typeof ctx[name], 'function', `${name}() missing from wifi_diag.html`);
    }
});

test('secs() renders durations a human reads at a glance', () => {
    const { secs } = loadPage();
    assert.equal(secs(0), '0s');
    assert.equal(secs(45), '45s');
    assert.equal(secs(60), '1m 0s');
    assert.equal(secs(3599), '59m 59s');
    assert.equal(secs(3600), '1h 0m');
    assert.equal(secs(7265), '2h 1m');
    // Uptime counters are unsigned on the device; a negative can only arrive from a malformed
    // response, and must not render as "-1s" next to real numbers.
    assert.equal(secs(-5), '0s');
});

test('bytes() switches unit at the right places', () => {
    const { bytes } = loadPage();
    assert.equal(bytes(0), '0 B');
    assert.equal(bytes(1023), '1023 B');
    assert.equal(bytes(1024), '1.0 KB');
    assert.equal(bytes(32768), '32.0 KB');
    assert.equal(bytes(1048576), '1.0 MB');
});

test('rssiClass() buckets signal the way a radio actually behaves', () => {
    const { rssiClass } = loadPage();
    assert.equal(rssiClass(-40), 'good');
    assert.equal(rssiClass(-65), 'good');   // boundary: still comfortable
    assert.equal(rssiClass(-66), 'warn');
    assert.equal(rssiClass(-75), 'warn');   // boundary: workable, already costing rate
    assert.equal(rssiClass(-76), 'bad');
    assert.equal(rssiClass(-90), 'bad');
});

test('page and firmware agree on what counts as a weak signal', () => {
    // wd_hist_bucket() in wifi_diag.c splits at -55/-65/-75/-85, and the findings rule warns from
    // -72 and errors from -80. The page's amber boundary must line up with the firmware's
    // "workable" bucket edge (-65) and its red boundary with the "weak" edge (-75) -- otherwise the
    // colour on screen contradicts the histogram in the downloaded report for the same reading.
    const c = readFileSync(IMPL, 'utf8');
    const bucket = c.match(/static int wd_hist_bucket\(int8_t rssi\)\s*\{([\s\S]*?)\n\}/);
    assert.ok(bucket, 'wd_hist_bucket() not found in wifi_diag.c -- did it get renamed?');
    const edges = [...bucket[1].matchAll(/rssi >= (-\d+)/g)].map(m => Number(m[1]));
    assert.deepEqual(edges, [-55, -65, -75, -85],
        'histogram bucket edges changed in wifi_diag.c; update rssiClass() in wifi_diag.html to match');

    const { rssiClass } = loadPage();
    assert.equal(rssiClass(edges[1]), 'good', 'page must call the "good" bucket edge good');
    assert.equal(rssiClass(edges[1] - 1), 'warn');
    assert.equal(rssiClass(edges[2]), 'warn', 'page must call the "workable" bucket edge amber');
    assert.equal(rssiClass(edges[2] - 1), 'bad');
});

test('the event timeline cannot leak identifiers into a masked report', () => {
    // The report's header promises "masked -- safe to paste in public", and the RADIO section keeps
    // that promise. The EVENT TIMELINE did not: identifiers were formatted into each ring entry at
    // emit time, and the report printed the ring verbatim, so every timeline line carried the full
    // SSID and the router's full BSSID under a header saying otherwise.
    //
    // The fix is structural -- identifiers live in their own fields on wd_evt_t and are applied by
    // wd_evt_render(), which masks unless raw. This test pins the structure, because the failure is
    // silent: the report still looks masked at a glance, since the leak is fifty lines below the
    // part that is masked correctly.
    const c = readFileSync(IMPL, 'utf8');

    const pushes = [...c.matchAll(/wd_evt_push\(([^;]*?)\);/gs)].map(m => m[1]);
    assert.ok(pushes.length >= 5, `expected the wd_evt_push() call sites, found ${pushes.length}`);
    for (const args of pushes) {
        // Everything from the third argument on is the format string and its arguments.
        const fmt = args.slice(args.indexOf(',', args.indexOf(',') + 1) + 1);
        assert.ok(!/ssid|bssid/i.test(fmt),
            `a wd_evt_push() format string mentions an identifier: ${fmt.trim().slice(0, 80)}. ` +
            `Pass it as the ssid/bssid argument instead, or it will bypass masking.`);
    }

    assert.ok(/wd_evt_render\(&e, raw,/.test(c),
        'the report timeline must render through wd_evt_render() with the request\'s raw flag');
    assert.ok(/wd_evt_render\(&e, true,/.test(c),
        'the JSON timeline should render unmasked -- it is read on the owner\'s own device');
});

test('the low-memory threshold is the same number in both places', () => {
    // The page paints "largest block" red below WD_LOW_BLOCK_BYTES; the firmware raises a finding
    // at the same point. Two different numbers would mean the page and the report disagree about
    // whether the device is short of the internal RAM its Wi-Fi transmit buffers come from.
    //
    // This threshold was 32768 when the feature first shipped, which no healthy unit could ever
    // satisfy -- a device in the field runs with ~30 KB of internal RAM free in total, so the
    // finding fired on every report and the page painted permanently red. Read the value rather
    // than hardcoding it here, so a future retune only has to touch the two source files.
    const c = readFileSync(IMPL, 'utf8');
    const def = c.match(/#define WD_LOW_BLOCK_BYTES\s+(\d+)/);
    assert.ok(def, 'WD_LOW_BLOCK_BYTES not found in wifi_diag.c -- did it get renamed?');
    const limit = Number(def[1]);

    assert.ok(limit >= 2048 && limit < 30000,
        `WD_LOW_BLOCK_BYTES is ${limit}: it must exceed a couple of ~1.6 KB TX buffers, and must ` +
        `stay well under the ~30 KB of internal RAM a healthy device has free in total -- ` +
        `otherwise the finding can never stop firing`);
    assert.ok(new RegExp(`block_min < WD_LOW_BLOCK_BYTES`).test(c),
        'the low-memory finding no longer compares against WD_LOW_BLOCK_BYTES');

    const html = readFileSync(PAGE, 'utf8');
    assert.ok(html.includes(`int_block < ${limit}`) && html.includes(`int_block_min < ${limit}`),
        `wifi_diag.html must flag a largest-free-block below ${limit} to match WD_LOW_BLOCK_BYTES`);
});
