// Load main/web/src/main.js into a Node sandbox so its pure logic can be tested
// without a browser.
//
// Why this exists: main.js holds several pairs of functions that MUST be exact
// inverses of each other -- parsePidText/composePidText, abcToBn/bnToAbc,
// emitSensorsYaml/parseSensorsYaml. When one drifts, a user's stored config
// stops round-tripping byte-identically, which is silent: the page still
// renders, the device still polls, and the damage shows up as a changed
// auto_pid.json days later. Those are exactly the properties a test can pin
// and a human reviewer cannot.
//
// main.js is a plain browser script (no import/export, no bundler) embedded in
// the firmware image as a binary blob and served at /main.js. So we evaluate it
// verbatim in a vm context with just enough DOM to survive load, and then read
// the functions out.
//
//     node --test tools/webtest/
//
// Zero dependencies -- node:test and node:assert only.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import vm from 'node:vm';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
export const MAIN_JS = join(REPO, 'main', 'web', 'src', 'main.js');

// Names to lift out of the sandbox. `function f(){}` declarations land on the
// context object by themselves, but `const f = () => {}` creates a lexical
// binding that does not -- and most of the interesting helpers are consts. So
// we append an epilogue that runs in the script's own scope and copies them.
//
// Add a name here to make it testable. If a name disappears from main.js the
// epilogue records it as missing rather than throwing, so one rename surfaces
// as a failing test that names the symbol instead of a dead harness.
const EXPORTS = [
    // --- stored PID wire string <-> UI fields (issues #31, #51) ---
    'PID_MODES', 'MODE_IDENT_LEN', 'MODE_ECHO_BYTES', 'PID_TEXT_MAX',
    'EXPRESSIBLE_MODES', 'PID_HINT_DEFAULT', 'RMBA_MAX_SIZE',
    'pidServiceMode', 'parsePidText', 'composePidText',
    'rmbaSplitIdent', 'rmbaJoinIdent', 'rmbaIdentProblem',
    // --- expression form: friendly A/B/C <-> stored Bn (issues #61, #51) ---
    'exprDataOffset', 'abcToBn', 'bnToAbc', 'exprAsAbc', 'exprHasWireByte',
    'rangeToArith',
    // --- sensor-set file (issue #63) ---
    // NB: docs/internals/web_ui.md calls the wire-vs-friendly predicate
    // `sensorExprIsWire()`. No such symbol exists -- the real one is
    // `exprHasWireByte` (main.js:979). The doc is wrong; fix it, don't chase it.
    'emitSensorsYaml', 'parseSensorsYaml', 'sensorPidParts',
    // --- sweep prediction (issues #29, #67) ---
    'effDivisor', 'readSampleEvery', 'sampleLoadFromEntries', 'predictedSweepMs',
    // --- datalogger auto-start visibility ---
    'csvAutostartLabel',
];

function stubElement() {
    const el = {
        style: {}, dataset: {}, children: [], value: '', textContent: '',
        innerHTML: '', checked: false, disabled: false, files: [],
        classList: { add() {}, remove() {}, toggle() {}, contains: () => false },
        appendChild() {}, removeChild() {}, remove() {}, insertAdjacentHTML() {},
        addEventListener() {}, removeEventListener() {}, dispatchEvent() {},
        setAttribute() {}, getAttribute: () => null, removeAttribute: () => {},
        querySelector: () => null, querySelectorAll: () => [], closest: () => null,
        focus() {}, blur() {}, click() {}, scrollIntoView() {},
    };
    return el;
}

// Ids the real page actually has: id="..." in homepage_full.html, plus ids main.js
// assigns to elements it builds itself. Same two sources tools/lint_web.py check 1 uses,
// so the test and the lint agree on what "exists" means.
function realPageIds() {
    const html = readFileSync(join(REPO, 'main', 'web', 'homepage_full.html'), 'utf8');
    const js = readFileSync(MAIN_JS, 'utf8');
    const ids = new Set();
    for (const m of html.matchAll(/\bid=["']([^"']+)["']/g)) ids.add(m[1]);
    for (const m of js.matchAll(/\.id\s*=\s*["']([^"']+)["']/g)) ids.add(m[1]);
    for (const m of js.matchAll(/setAttribute\(\s*["']id["']\s*,\s*["']([^"']+)["']\s*\)/g)) ids.add(m[1]);
    return ids;
}

// The page's <select>s with their static <option>s, and the first id carrying each class, so
// strictDom can answer the two things Load() and postConfig() actually rely on:
//  - Load() sets a select by selectedIndex and postConfig() reads it back by .value. A plain stub
//    keeps the two unrelated, so a config that loaded "ble_status: enable" would Submit "" -- and a
//    test of the load -> Submit round trip would be checking the stub, not the page.
//  - Load() ends with document.querySelector(".store").disabled = true. Returning null there (the
//    permissive default) makes every config load throw on its last line.
function realPageSelectsAndClasses() {
    const html = readFileSync(join(REPO, 'main', 'web', 'homepage_full.html'), 'utf8');
    const selects = new Map();
    for (const m of html.matchAll(/<select\b([^>]*)>([\s\S]*?)<\/select>/g)) {
        const id = /\bid=["']([^"']+)["']/.exec(m[1]);
        if (!id) continue;
        const options = [...m[2].matchAll(/<option\b([^>]*)>([\s\S]*?)<\/option>/g)].map(o => {
            const v = /\bvalue=["']([^"']*)["']/.exec(o[1]);
            return { value: v ? v[1] : o[2].trim(), text: o[2].trim(), selected: /\bselected\b/.test(o[1]) };
        });
        selects.set(id[1], options);
    }
    const classIds = new Map();
    for (const m of html.matchAll(/<[a-zA-Z][^>]*>/g)) {
        const id = /\bid=["']([^"']+)["']/.exec(m[0]);
        const cls = /\bclass=["']([^"']+)["']/.exec(m[0]);
        if (!id || !cls) continue;
        for (const c of cls[1].split(/\s+/)) if (c && !classIds.has(c)) classIds.set(c, id[1]);
    }
    return { selects, classIds };
}

// A <select> whose value and selectedIndex move together, as in a browser: setting a value no
// option carries leaves it at "" (selectedIndex -1), not at the string that was assigned. A select
// the HTML gives no static options (the time-zone list, filled by main.js at run time) gets an
// empty options list and a free-form value, because this stub's appendChild does not add options.
function selectElement(options) {
    const el = stubElement();
    el.options = options.map(o => ({ value: o.value, text: o.text }));
    if (!options.length) return el;
    let idx = options.findIndex(o => o.selected);
    if (idx < 0) idx = 0;
    Object.defineProperty(el, 'selectedIndex', {
        enumerable: true,
        get: () => idx,
        set(v) {
            const n = Number(v);
            idx = (Number.isInteger(n) && n >= 0 && n < el.options.length) ? n : -1;
        },
    });
    Object.defineProperty(el, 'value', {
        enumerable: true,
        get: () => (idx >= 0 ? el.options[idx].value : ''),
        set(v) { idx = el.options.findIndex(o => o.value === String(v)); },
    });
    return el;
}

/**
 * Evaluate main.js and return { exports, missing, context }.
 *  - exports: the EXPORTS above that resolved, by name
 *  - missing: names that did not resolve (a rename or deletion in main.js)
 *
 * Options:
 *  - strictDom: getElementById returns null for an id the real page does not define.
 */
export function loadMainJs({ strictDom = false } = {}) {
    const src = readFileSync(MAIN_JS, 'utf8');

    // strictDom: model the REAL page -- an id that homepage_full.html does not define
    // returns null, exactly as the browser does. The permissive default hands out a stub
    // for every id, which is fine for the pure round-trip tests but cannot catch a DOM
    // element that was deleted from the HTML while main.js still looks it up.
    const knownIds = strictDom ? realPageIds() : null;
    const page = strictDom ? realPageSelectsAndClasses() : null;

    const elCache = new Map();
    const documentStub = {
        // Return a stable stub per id rather than null: main.js has exactly one
        // top-level DOM side effect -- getElementById("defaultOpen").click() --
        // and a null there aborts the whole load.
        getElementById(id) {
            if (knownIds && !knownIds.has(id)) return null;
            if (!elCache.has(id)) {
                const opts = page && page.selects.get(id);
                elCache.set(id, opts ? selectElement(opts) : stubElement());
            }
            return elCache.get(id);
        },
        // strictDom answers the two simple selector shapes, "#id" and ".class"; anything else is
        // null, as in the permissive default. ".class" is an approximation: it returns the first
        // element that has an id AND the class (the stub can only hand out elements by id), where
        // a browser returns the first element with the class, id or not. Exact while the page's
        // only queried class, .store, sits on a single element (#custom_pid_store).
        querySelector(sel) {
            if (!page) return null;
            let m = /^#([\w-]+)$/.exec(sel);
            if (m) return documentStub.getElementById(m[1]);
            m = /^\.([\w-]+)$/.exec(sel);
            if (m && page.classIds.has(m[1])) {
                return documentStub.getElementById(page.classIds.get(m[1]));
            }
            return null;
        },
        querySelectorAll: () => [],
        createElement: () => stubElement(),
        createTextNode: () => ({}),
        addEventListener() {},
        removeEventListener() {},
        body: stubElement(),
        head: stubElement(),
        documentElement: stubElement(),
    };

    const ctx = {
        document: documentStub,
        console,
        navigator: { userAgent: 'node', clipboard: { writeText: async () => {} } },
        location: { href: 'http://device/', protocol: 'http:', host: 'device', reload() {} },
        localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
        sessionStorage: { getItem: () => null, setItem() {}, removeItem() {} },
        // Any test that needs the network should stub this itself; the default
        // must never reach out.
        fetch: async () => { throw new Error('sandbox: unexpected network call'); },
        // Under strictDom the tests call showNotification(), which arms a 5 s (50 s for the
        // default-password message) timer. An un-unref'd timer keeps `node --test` alive that
        // long, so drop the process's reference to it. Behaviour is otherwise unchanged.
        //
        // ⚠️ An unref'd timer does NOT keep node alive, so a future strictDom test that AWAITS
        // a timer can see the process exit before the callback runs and pass for the wrong
        // reason. config_contract.test.mjs awaits postConfig(), which waits on timers, so it
        // replaces context.setTimeout with a ref'd 0 ms one; do the same in any test that
        // waits on a timeout, rather than removing the unref. Note requestAnimationFrame below
        // closes over Node's real setTimeout, not this wrapper, so rAF timers are never unref'd
        // (harmless at 0 ms).
        setTimeout: strictDom
            ? (fn, ms, ...a) => { const t = setTimeout(fn, ms, ...a); t.unref?.(); return t; }
            : setTimeout,
        clearTimeout, setInterval, clearInterval, queueMicrotask,
        requestAnimationFrame: (f) => setTimeout(f, 0),
        alert() {}, confirm: () => true, prompt: () => null,
        Blob: class Blob { constructor(parts) { this.parts = parts; } },
        File: class File {},
        FileReader: class FileReader {},
        FormData: class FormData { append() {} },
        XMLHttpRequest: class XMLHttpRequest {},
        URL: { createObjectURL: () => 'blob:stub', revokeObjectURL() {} },
        TextEncoder, TextDecoder,
    };
    ctx.window = ctx;
    ctx.globalThis = ctx;
    vm.createContext(ctx);

    const epilogue = `
;globalThis.__harness = {};
${EXPORTS.map(n => `try { globalThis.__harness[${JSON.stringify(n)}] = ${n}; } catch (e) {}`).join('\n')}
`;

    vm.runInContext(src + epilogue, ctx, { filename: 'main.js' });

    const exported = ctx.__harness || {};
    const missing = EXPORTS.filter(n => exported[n] === undefined);
    return { exports: exported, missing, context: ctx };
}

/** Convenience: load once per test file and fail loudly on a missing symbol. */
export function requireExports(...names) {
    const { exports, missing } = loadMainJs();
    const need = names.filter(n => missing.includes(n));
    if (need.length) {
        throw new Error(
            `main.js no longer exports: ${need.join(', ')}. ` +
            `Either the function was renamed (update tools/webtest/sandbox.mjs EXPORTS) ` +
            `or it was deleted (update the tests that cover it).`);
    }
    return exports;
}
