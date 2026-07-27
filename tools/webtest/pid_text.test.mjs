// The stored-PID wire string <-> UI fields contract (issues #31, #51).
//
// parsePidText/composePidText must be EXACT inverses on canonical rows. If they
// drift, a user's auto_pid.json stops round-tripping byte-identically through a
// load/save cycle -- silently, because the page still renders and the device
// still polls whatever bytes ended up stored.

import test from 'node:test';
import assert from 'node:assert/strict';
import { requireExports } from './sandbox.mjs';

// Values built inside the vm have that realm's Object/Array prototypes, so
// assert.deepStrictEqual rejects them as "same structure but not
// reference-equal". Normalise before structural comparison.
const plain = v => JSON.parse(JSON.stringify(v));

const {
    PID_MODES, MODE_IDENT_LEN, MODE_ECHO_BYTES, PID_TEXT_MAX, EXPRESSIBLE_MODES,
    RMBA_MAX_SIZE, pidServiceMode, parsePidText, composePidText,
    rmbaSplitIdent, rmbaJoinIdent, rmbaIdentProblem,
} = requireExports(
    'PID_MODES', 'MODE_IDENT_LEN', 'MODE_ECHO_BYTES', 'PID_TEXT_MAX',
    'EXPRESSIBLE_MODES', 'RMBA_MAX_SIZE', 'pidServiceMode', 'parsePidText',
    'composePidText', 'rmbaSplitIdent', 'rmbaJoinIdent', 'rmbaIdentProblem');

test('PID_MODES is the single source for the derived lookups', () => {
    assert.deepEqual(plain(MODE_IDENT_LEN), { '01': 2, '22': 4, '23': 12 });
    // Mode 23 sends a 6-byte operand and echoes NONE of it. This is the fact
    // the pre-#51 code got wrong by deriving echo from identifier length.
    assert.deepEqual(plain(MODE_ECHO_BYTES), { '01': 1, '22': 2, '23': 0 });
});

test('EXPRESSIBLE_MODES holds the right set -- but NOT in written order', () => {
    // Object.keys puts canonical array indices first in ascending numeric order,
    // so '22' and '23' precede '01' even though PID_MODES lists '01' first.
    // Harmless today: the Mode dropdown's <option>s are hardcoded in the row
    // template (main.js ~1064) and this array is only used for membership tests
    // and a console.warn join. Pinned so nobody later generates the dropdown
    // from it and ships a 22 / 23 / 01 option order.
    assert.deepEqual(plain(EXPRESSIBLE_MODES), ['22', '23', '01']);
    assert.deepEqual([...EXPRESSIBLE_MODES].sort(), ['01', '22', '23']);
});

test('PID_TEXT_MAX is derived, not hardcoded', () => {
    // 2 (service) + widest identifier (12, mode 23) + 1 (frames hint) = 15.
    // It was hardcoded to 10 before #51, which silently rejected every mode-23
    // row on Store. Widening PID_MODES must move this automatically.
    assert.equal(PID_TEXT_MAX, 15);
    assert.equal(PID_TEXT_MAX, 2 + Math.max(...Object.values(MODE_IDENT_LEN)) + 1);
    assert.equal('23FFFFAC1800041'.length, PID_TEXT_MAX);
});

test('parsePidText / composePidText are exact inverses on canonical rows', () => {
    const canonical = [
        '010C1',            // mode 01, ident 0C, hint 1
        '010C',             // no hint
        '2217461',          // mode 22, ident 1746, hint 1
        '221746',
        '23FFFFAC1800041',  // mode 23, addr FFFFAC18 size 0004, hint 1
        '23FFFFAC08000 1'.replace(' ', ''),
    ];
    for (const wire of canonical) {
        const p = parsePidText(wire);
        assert.notEqual(p, null, `expected ${wire} to parse as canonical`);
        assert.equal(composePidText(p.service, p.ident, p.hint), wire,
            `round-trip changed ${wire}`);
    }
});

test('identifier and hint are preserved verbatim, including case', () => {
    // compose must reproduce the STORED bytes, so parse must not case-fold.
    const p = parsePidText('22aBcD1');
    assert.deepEqual(plain(p), { service: '22', ident: 'aBcD', hint: '1' });
    assert.equal(composePidText(p.service, p.ident, p.hint), '22aBcD1');
});

test('non-canonical shapes return null and take the legacy verbatim path', () => {
    const notCanonical = [
        '',                  // empty
        'ZZ0C1',             // non-hex
        '030C1',             // service outside PID_MODES
        '010C12',            // too long for mode 01 (ident 2 + hint 1)
        '2217',              // too short for mode 22
        '23FFFFAC1800001',   // mode 23 size 0 -- outside 1..RMBA_MAX_SIZE
        '23FFFFAC1800071',   // mode 23 size 7 -- needs multi-frame, refused
    ];
    for (const wire of notCanonical) {
        assert.equal(parsePidText(wire), null, `expected ${wire} to be non-canonical`);
    }
});

test('an out-of-range mode-23 size is non-canonical, not an exception', () => {
    // Regression guard for the #66 review finding: a canonical-looking mode 23
    // row with a bad size used to block every save of the WHOLE page, so a user
    // could not save an unrelated edit without first deleting that row.
    for (const size of [0, 7, 255]) {
        const wire = '23FFFFAC18' + size.toString(16).padStart(4, '0') + '1';
        assert.doesNotThrow(() => parsePidText(wire));
        assert.equal(parsePidText(wire), null);
    }
    for (let size = 1; size <= RMBA_MAX_SIZE; size++) {
        const wire = '23FFFFAC18' + size.toString(16).padStart(4, '0') + '1';
        assert.notEqual(parsePidText(wire), null, `size ${size} should be canonical`);
    }
});

test('rmbaSplitIdent / rmbaJoinIdent are exact inverses for every legal size', () => {
    for (let size = 1; size <= RMBA_MAX_SIZE; size++) {
        const ident = rmbaJoinIdent('FFFFAC18', size);
        assert.equal(ident.length, 12);
        const back = rmbaSplitIdent(ident);
        assert.equal(back.addr, 'FFFFAC18');
        assert.equal(back.size, size);
        assert.equal(rmbaJoinIdent(back.addr, back.size), ident);
    }
});

test('rmbaIdentProblem returns null only for a usable operand', () => {
    assert.equal(rmbaIdentProblem('FFFFAC18', 4), null);
    assert.equal(rmbaIdentProblem('ffffac18', 1), null);  // case-insensitive
    for (const bad of ['FFFFAC1', 'FFFFAC188', 'GGGGAC18', '', 'FFFF AC18']) {
        assert.notEqual(rmbaIdentProblem(bad, 4), null, `address ${bad} should be rejected`);
    }
    for (const size of [0, 7, -1]) {
        assert.notEqual(rmbaIdentProblem('FFFFAC18', size), null,
            `size ${size} should be rejected`);
    }
});

test('pidServiceMode defaults to 01 rather than throwing', () => {
    assert.equal(pidServiceMode('010C1'), '01');
    assert.equal(pidServiceMode('2217461'), '22');
    assert.equal(pidServiceMode('23FFFFAC1800041'), '23');
    assert.equal(pidServiceMode('ab12'), 'AB');   // upper-cased, still returned
    assert.equal(pidServiceMode(''), '01');
    assert.equal(pidServiceMode(null), '01');
    assert.equal(pidServiceMode(undefined), '01');
});
