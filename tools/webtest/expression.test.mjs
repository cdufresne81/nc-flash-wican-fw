// Friendly A/B/C expression form <-> stored raw Bn indices (issues #61, #51).
//
// The UI edits expressions in the standard SAE J1979 / Torque vocabulary (data
// bytes A, B, C... counting from the first DATA byte) while auto_pid.json keeps
// the firmware's raw Bn indices, which include ISO-TP framing. The two maps must
// be exact inverses on the standard subset, or a load/save cycle silently
// rewrites a user's expression -- and a wrong expression logs a plausible number
// forever rather than failing.

import test from 'node:test';
import assert from 'node:assert/strict';
import { requireExports } from './sandbox.mjs';

const {
    exprDataOffset, abcToBn, bnToAbc, exprAsAbc, exprHasWireByte, rangeToArith,
} = requireExports('exprDataOffset', 'abcToBn', 'bnToAbc', 'exprAsAbc',
                   'exprHasWireByte', 'rangeToArith');

test('exprDataOffset is driven by ECHO bytes, not identifier length', () => {
    // B0 = ISO-TP PCI, B1 = service echo, then the echoed operand.
    // Mode 01 echoes 1 byte  -> data at B3
    // Mode 22 echoes 2 bytes -> data at B4
    // Mode 23 echoes 0 bytes -> data at B2   <-- the #51 correction
    assert.equal(exprDataOffset('01'), 3);
    assert.equal(exprDataOffset('22'), 4);
    assert.equal(exprDataOffset('23'), 2);
    // Deriving from identifier length would have given mode 23 an offset of 8
    // (2 + 6 operand bytes) and read past the end of the frame.
    assert.notEqual(exprDataOffset('23'), 8);
    assert.equal(exprDataOffset('09'), null);       // unknown mode
    assert.equal(exprDataOffset(undefined), null);
});

test('an unknown mode makes both maps identity, never a wrong translation', () => {
    const off = exprDataOffset('09');
    assert.equal(abcToBn('A*2', off), 'A*2');
    assert.equal(bnToAbc('B3*2', off), 'B3*2');
});

test('abcToBn / bnToAbc are exact inverses on the standard subset', () => {
    const cases = [
        ['01', 'A'],
        ['01', 'A*0.5'],
        ['01', 'A-40'],
        ['01', '((A*256)+B)/4'],
        ['01', '(A*256+B)*0.0625'],
        ['22', '((A*256)+B)*0.0625'],
        ['22', 'A'],
        ['23', 'A'],
        ['23', '((A*256)+B)'],
    ];
    for (const [mode, abc] of cases) {
        const off = exprDataOffset(mode);
        const bn = abcToBn(abc, off);
        assert.equal(bnToAbc(bn, off), abc, `mode ${mode}: ${abc} -> ${bn} -> did not return`);
        assert.equal(abcToBn(bnToAbc(bn, off), off), bn, `mode ${mode}: ${bn} not stable`);
    }
});

test('the same friendly expression re-frames across modes', () => {
    // This is the point of the friendly form: it is mode-independent, so
    // changing a row's Mode re-frames the stored bytes automatically.
    assert.equal(abcToBn('((A*256)+B)/4', exprDataOffset('01')), '((B3*256)+B4)/4');
    assert.equal(abcToBn('((A*256)+B)/4', exprDataOffset('22')), '((B4*256)+B5)/4');
    assert.equal(abcToBn('((A*256)+B)/4', exprDataOffset('23')), '((B2*256)+B3)/4');
});

test('the float token FA binds the letter to F, not to a bare data byte', () => {
    // VOLEFF / VOLFLOW are IEEE-754 float32; "FA" means float32 STARTING at
    // data byte A. Without this the 4-byte read logs the raw bit pattern
    // (1.0f reading as 1065353216) -- silently wrong rather than failing.
    assert.equal(abcToBn('FA', exprDataOffset('23')), 'F2');
    assert.equal(bnToAbc('F2', exprDataOffset('23')), 'FA');
    assert.equal(abcToBn('FA', exprDataOffset('01')), 'F3');
    assert.equal(bnToAbc('F3', exprDataOffset('01')), 'FA');
    assert.equal(abcToBn('FA*100', exprDataOffset('23')), 'F2*100');
});

test('Fn earns a letter only when all four of its bytes are in frame', () => {
    // poll_log evaluates ONE 8-byte frame, so a float at B5 would span B5..B8
    // and read past the end. It must stay raw rather than be prettified into a
    // name that masks the out-of-bounds read.
    const off = exprDataOffset('01');                 // 3
    assert.equal(bnToAbc('F3', off), 'FA');           // 3..6 in frame
    assert.equal(bnToAbc('F4', off), 'FB');           // 4..7 in frame
    assert.equal(bnToAbc('F5', off), 'F5');           // 5..8 -> out of frame, stays raw
    assert.equal(bnToAbc('F0', off), 'F0');           // before the data window
});

test('out-of-window and signed byte references stay raw', () => {
    const off = exprDataOffset('01');                 // 3
    assert.equal(bnToAbc('B8', off), 'B8');           // past the 8-byte frame
    assert.equal(bnToAbc('B0', off), 'B0');           // framing byte, before the window
    assert.equal(bnToAbc('S3', off), 'S3');           // signed token is not translated
    assert.equal(bnToAbc('B7', off), 'E');            // last in-frame byte does translate
});

test('exprAsAbc returns null unless the row round-trips exactly', () => {
    const off01 = exprDataOffset('01');
    assert.equal(exprAsAbc('((B3*256)+B4)/4', off01), '((A*256)+B)/4');
    assert.equal(exprAsAbc('B3-40', off01), 'A-40');
    // Not representable -> null -> the row stays in raw Bn, edited verbatim.
    assert.equal(exprAsAbc('S3-40', off01), null);          // signed
    assert.equal(exprAsAbc('B8*2', off01), null);           // out of frame
    assert.equal(exprAsAbc('B0*2', off01), null);           // framing byte
    assert.equal(exprAsAbc('B3-40', null), null);           // unknown mode
});

test('exprHasWireByte is the one all-or-nothing wire/friendly predicate', () => {
    // NB: docs/internals/web_ui.md calls this sensorExprIsWire(). That symbol
    // does not exist -- the doc is wrong.
    assert.equal(exprHasWireByte('B3'), true);
    assert.equal(exprHasWireByte('S3'), true);
    assert.equal(exprHasWireByte('F2'), true);
    assert.equal(exprHasWireByte('A'), false);
    assert.equal(exprHasWireByte('((A*256)+B)/4'), false);
    assert.equal(exprHasWireByte('FA'), false);
    // A hand edit mixing conventions is taken as wire form, whole-row.
    assert.equal(exprHasWireByte('A*256+B3'), true);
});

test('rangeToArith normalises compact ranges and is idempotent', () => {
    assert.equal(rangeToArith('[B3:B4]/4'), '((B3*256)+B4)/4');
    assert.equal(rangeToArith('[B3:B3]'), 'B3');                  // 1-byte range is the byte
    assert.equal(rangeToArith('[B3:B5]'), '((B3*65536)+(B4*256)+B5)');
    // Idempotent: a migrated config then round-trips byte-identically.
    const once = rangeToArith('[B3:B4]/4');
    assert.equal(rangeToArith(once), once);
    // Left compact: signed, reversed, or wider than 4 bytes.
    assert.equal(rangeToArith('[S3:S4]/4'), '[S3:S4]/4');
    assert.equal(rangeToArith('[B4:B3]'), '[B4:B3]');
    assert.equal(rangeToArith('[B0:B7]'), '[B0:B7]');
});
