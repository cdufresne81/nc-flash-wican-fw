// The recorder card's auto-start label.
//
// Three different situations used to render identically as "Idle", and a user acting on
// that ambiguity caused a real field failure -- see docs/internals/csv_logger.md. The
// firmware now reports an autostart state; this is the half that says it out loud.

import test from 'node:test';
import assert from 'node:assert/strict';
import { requireExports } from './sandbox.mjs';

const { csvAutostartLabel } = requireExports('csvAutostartLabel');

// Values built inside the vm carry that realm's prototypes, so deepStrictEqual rejects
// them as "same structure but not reference-equal". Normalise, as pid_text.test.mjs does.
const plain = v => (v === null ? null : JSON.parse(JSON.stringify(v)));

test('a running or unknown datalogger has nothing extra to say', () => {
    assert.equal(csvAutostartLabel(null), null);
    assert.equal(csvAutostartLabel({}), null);
    assert.equal(csvAutostartLabel({ autostart: 'ready' }), null);
    assert.equal(csvAutostartLabel({ autostart: 'disabled' }), null);
});

test('the boot holdoff counts down in whole seconds, rounding up', () => {
    // Rounded UP so the label never says "0s" while the device is still holding off.
    assert.deepEqual(plain(csvAutostartLabel({ autostart: 'holdoff', autostart_in_ms: 10000 })),
                     { text: 'Starting in 10s', warn: false });
    assert.deepEqual(plain(csvAutostartLabel({ autostart: 'holdoff', autostart_in_ms: 4200 })),
                     { text: 'Starting in 5s', warn: false });
    assert.deepEqual(plain(csvAutostartLabel({ autostart: 'holdoff', autostart_in_ms: 1 })),
                     { text: 'Starting in 1s', warn: false });
    // Missing/!=number countdown must not render NaN.
    assert.deepEqual(plain(csvAutostartLabel({ autostart: 'holdoff' })),
                     { text: 'Starting in 0s', warn: false });
});

test('a writer that failed to start is distinct from a guard skip', () => {
    // Different cause, different recovery: "power-cycle to clear the RTC guard" is the
    // wrong advice for an out-of-memory start failure, so the two must not share a label.
    const u = csvAutostartLabel({ autostart: 'unavailable' });
    assert.equal(u.warn, true);
    assert.match(u.text, /did not start/);
    assert.notEqual(u.text, csvAutostartLabel({ autostart: 'skipped' }).text);
});

test('a skipped bring-up is a warning, and says whether it will retry', () => {
    const retry = csvAutostartLabel({ autostart: 'skipped_retry', autostart_in_ms: 60000 });
    assert.equal(retry.warn, true);
    assert.match(retry.text, /retrying in 60s/);

    const done = csvAutostartLabel({ autostart: 'skipped' });
    assert.equal(done.warn, true);
    // The two recoveries a user can actually perform, both named.
    assert.match(done.text, /Start/);
    assert.match(done.text, /power-cycle/);
});
