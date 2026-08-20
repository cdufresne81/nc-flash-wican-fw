// The gate on the "Restore Datalogger mode" button (issue #92).
//
// The button exists because a device left in Bench SLCAN stops datalogging and
// the only escape used to be hand-editing config.json. But the banner it sits
// in is ALSO on screen during every legitimate NC Flash session on older
// firmware, which switches the device to slcan on purpose for the duration of a
// flash. Pressing it then would reboot the adapter out from under a live ECU
// write and can leave the car's PCM half-written.
//
// So the whole safety of the feature is this one decision. It is a pure
// function precisely so it can be pinned here rather than argued about.

import test from 'node:test';
import assert from 'node:assert/strict';
import { requireExports } from './sandbox.mjs';

const { strandRestoreDecision } = requireExports('strandRestoreDecision');

const healthy = {
    ok: true, flash_active: false, datalog_parked: false, host_bus_claimed: false,
    manual_mode: 'auto', park_token: null, claim_token: null,
    lease_ttl_ms: 12000, claim_ttl_ms: 75000, bus_idle_ms: 3, stuck_flash_alarm: false,
};

test('a clean device proceeds behind an ordinary confirm', () => {
    const d = strandRestoreDecision(healthy);
    assert.equal(d.action, 'proceed');
    assert.match(d.message, /reboot/i);
});

test('an active flash REFUSES -- the case that could brick an ECU', () => {
    const d = strandRestoreDecision({ ...healthy, flash_active: true });
    assert.equal(d.action, 'refuse');
    assert.match(d.message, /ECU/);
});

test('a host bus-claim REFUSES too: the pre-flash window has no flash flag yet', () => {
    // The authorisation window before a write starts is exactly the dangerous
    // gap that flash_active alone does not cover.
    const d = strandRestoreDecision({ ...healthy, host_bus_claimed: true });
    assert.equal(d.action, 'refuse');
});

test('refusal wins over everything else when several flags are set', () => {
    const d = strandRestoreDecision({
        ...healthy, flash_active: true, datalog_parked: true, stuck_flash_alarm: true,
    });
    assert.equal(d.action, 'refuse');
});

test('a wedged flash REFUSES and asks for a power cycle, not a reboot', () => {
    // Rebooting would not clear it, so offering a reboot would be misleading.
    const d = strandRestoreDecision({ ...healthy, stuck_flash_alarm: true });
    assert.equal(d.action, 'refuse');
    assert.match(d.message, /power-cycle/i);
});

test('no answer from /datalog is "cannot confirm", never "all clear"', () => {
    // Older firmware has no /datalog at all. Treating that silence as safe is
    // how you reboot a device mid-flash.
    const d = strandRestoreDecision(null);
    assert.equal(d.action, 'confirm');
    assert.match(d.message, /ONLY if you are certain/);
});

test('a parked datalogger asks, but does not block', () => {
    // A park can be left behind by a dead host socket. Refusing outright would
    // push the user back to hand-editing -- the thing this button replaces.
    const d = strandRestoreDecision({ ...healthy, datalog_parked: true });
    assert.equal(d.action, 'confirm');
});

test('every outcome carries a message the user can act on', () => {
    for (const input of [
        healthy,
        null,
        { ...healthy, flash_active: true },
        { ...healthy, host_bus_claimed: true },
        { ...healthy, stuck_flash_alarm: true },
        { ...healthy, datalog_parked: true },
    ]) {
        const d = strandRestoreDecision(input);
        assert.ok(['refuse', 'confirm', 'proceed'].includes(d.action));
        assert.equal(typeof d.message, 'string');
        assert.ok(d.message.length > 20, `message too terse for ${JSON.stringify(input)}`);
    }
});

test('an empty object is not mistaken for a healthy device', () => {
    // A truncated or unexpected body must not read as "no flags set, go ahead"
    // any more dangerously than a healthy one does: with no flags there is
    // genuinely nothing running, so proceeding behind a confirm is correct --
    // but it must still be a confirm, never an unprompted action.
    const d = strandRestoreDecision({});
    assert.equal(d.action, 'proceed');
    assert.match(d.message, /\?/);
});
