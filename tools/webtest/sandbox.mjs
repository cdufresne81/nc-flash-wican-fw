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

/**
 * Evaluate main.js and return { exports, missing, context }.
 *  - exports: the EXPORTS above that resolved, by name
 *  - missing: names that did not resolve (a rename or deletion in main.js)
 */
export function loadMainJs() {
    const src = readFileSync(MAIN_JS, 'utf8');

    const elCache = new Map();
    const documentStub = {
        // Return a stable stub per id rather than null: main.js has exactly one
        // top-level DOM side effect -- getElementById("defaultOpen").click() --
        // and a null there aborts the whole load.
        getElementById(id) {
            if (!elCache.has(id)) elCache.set(id, stubElement());
            return elCache.get(id);
        },
        querySelector: () => null,
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
        setTimeout, clearTimeout, setInterval, clearInterval, queueMicrotask,
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
