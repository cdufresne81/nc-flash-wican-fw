// Regression test for the dead "Submit changes" button (#141 fallout).
//
// PR #141 deleted the tcp_port_value / port_type / protocol fields from the UI but left
// validateForm() reading elements.tcpPortValue.value. That threw a TypeError before the
// line that enables the button ever ran, so the button was greyed out forever and no
// setting on the page could be saved. Load() swallowed the throw in a bare catch, so the
// console was clean on page load and nothing caught it -- not the lint, not the bench run,
// not the adversarial review.
//
// tools/lint_web.py check 4 now catches that exact shape statically. These tests cover what
// a reference check cannot see: that a valid form actually turns the button ON, and an
// invalid one turns it OFF. They run against a strict DOM, so an id deleted from
// homepage_full.html while main.js still looks it up shows up here too.

import test from 'node:test';
import assert from 'node:assert/strict';
import { loadMainJs } from './sandbox.mjs';

// A settings form the firmware would accept: every value validateForm() looks at.
function fillValid(doc) {
    const set = (id, v) => { doc.getElementById(id).value = v; };
    set('wifi_mode', 'APStation');
    set('ap_pass_value', 'not-the-default');   // 8..63 chars and not "@meatpi#"
    set('ssid_value', 'HomeNet');
    set('pass_value', 'stationpass');
    set('batt_alert_port', '1883');
    set('ble_status', 'disable');
    set('sleep_volt', '13.0');
    set('sleep_status', 'enable');
    set('sleep_disable_agree', 'no');
}

test('every getElements() target exists in the page', () => {
    const { context } = loadMainJs({ strictDom: true });
    for (const [key, el] of Object.entries(context.getElements())) {
        assert.ok(el, `getElements().${key} is null: its id is gone from homepage_full.html`);
    }
});

test('a valid form enables Submit (the #141 regression)', () => {
    const { context } = loadMainJs({ strictDom: true });
    const doc = context.document;
    fillValid(doc);
    const btn = doc.getElementById('submit_button');
    btn.disabled = true;            // the DOMContentLoaded state, main.js:100-103
    context.submit_enable();        // used to throw inside validateForm()
    assert.equal(btn.disabled, false, 'a valid form must enable Submit');
});

test('an invalid form disables Submit', () => {
    const { context } = loadMainJs({ strictDom: true });
    const doc = context.document;
    fillValid(doc);
    doc.getElementById('ap_pass_value').value = 'short';   // under the 8-char minimum
    const btn = doc.getElementById('submit_button');
    // Start from the OPPOSITE state of what we assert. The stub's default `disabled` is
    // false, so a test that only ever asserted false would pass even if submit_enable()
    // never touched the button at all.
    btn.disabled = false;
    context.submit_enable();
    assert.equal(btn.disabled, true, 'an invalid form must disable Submit');
});
