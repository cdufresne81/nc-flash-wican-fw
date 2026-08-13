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

test('the low-memory threshold is the same number in both places', () => {
    // The page paints "largest block" red below 32 KB; the firmware raises a finding at the same
    // point. Two different numbers here would mean the page and the report disagree about whether
    // the device is short of the internal RAM its Wi-Fi transmit buffers come from.
    const c = readFileSync(IMPL, 'utf8');
    assert.ok(/block_min != UINT32_MAX && block_min < 32768/.test(c),
        'the low-memory finding threshold moved in wifi_diag.c; update wifi_diag.html to match');
    const html = readFileSync(PAGE, 'utf8');
    assert.ok(/int_block < 32768/.test(html) && /int_block_min < 32768/.test(html),
        'wifi_diag.html no longer flags a largest-free-block below 32768');
});
