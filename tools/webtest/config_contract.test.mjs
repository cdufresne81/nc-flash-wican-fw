// The config.json contract between the firmware parser, its factory default, and the settings page
// (issue #148 cleanup).
//
// Three places must agree on config.json's keys, and nothing but these tests checks that they do:
//
//   - config_server_parse_cfg_into() (main/config_server.c). Some keys are REQUIRED: a missing one
//     fails the parse. At boot, config_server_load_cfg() answers a failed parse by deleting
//     config.json, writing device_config_default[] and rebooting -- the owner's Wi-Fi credentials,
//     BLE passkey and every setting are gone. /store_config runs the same parse before it writes
//     (issue #44), so a POST missing a required key is refused with HTTP 400 instead.
//   - device_config_default[], the factory default. If IT lacks a required key, the restore above
//     writes a file that fails the same parse: the device factory-resets on every boot, forever.
//   - postConfig() in main/web/src/main.js, which builds the Submit body. If it stops sending a
//     required key, every Submit is refused: no setting on the page can be saved.
//
// The #148 cleanup deleted keys from all three by hand. These tests pin the agreement so the next
// deletion (or the next "restore the old setting" revert) cannot get one of the three wrong.
//
// Retired keys are the other half. Retired means IGNORED: an old config.json still carries them, and
// it must load unchanged. So the parser must not look any of them up (a lookup is the first step
// towards a `goto config_error` on them), and neither the factory default nor Submit may write them
// back.

import test from 'node:test';
import assert from 'node:assert/strict';
import { loadMainJs } from './sandbox.mjs';
import { parserKeys, factoryDefaultText, numericDefine } from './config_source.mjs';

const PARSER = parserKeys();
const DEFAULT_TEXT = factoryDefaultText();
const AP_SSID_MIN = numericDefine('AP_SSID_MIN_LEN'), AP_SSID_MAX = numericDefine('AP_SSID_MAX_LEN');

// Keys retired by #148 (and the protocol/port shim before it). batt_alert* and batt_mqtt_* are
// matched as prefixes so a revived battery-alert key with a new suffix is caught too. wakeup_time
// never reached config.json (only a declared, undefined getter), but it is on the issue's list and
// costs nothing to hold out.
const RETIRED = ['periodic_wakeup', 'wakeup_interval', 'wakeup_time', 'log_filesystem', 'log_storage',
                 'protocol', 'port', 'port_type'];
const isRetired = (k) => RETIRED.includes(k) || /^batt_alert/.test(k) || /^batt_mqtt_/.test(k);

// Keys Submit may send although the parser does not read them, because something else on the
// device does. Currently empty, and that was checked rather than assumed: the other readers of
// config.json are sync_sys_time_apply_tz() (main/sync_sys_time.c), which takes "timezone" before
// the config server starts, and persist_debug_state() (components/cmdline/cmd_debug.c), which
// rewrites "debug" -- and the parser reads both. An entry here needs the name of the code that
// consumes it; "the UI sends it" is not a reason.
const CONSUMED_ELSEWHERE = new Map([
    // ['key', 'main/file.c: function() that reads it'],
]);

// The secret placeholder /load_config serves in place of a stored password
// (CONFIG_SECRET_PLACEHOLDER, config_server.c). The page must hand it back untouched; /store_config
// swaps the stored value back in.
const SECRET_PLACEHOLDER = '\x01stored\x01';

// Every required key must be present AND hold a value the parser accepts: a JSON string (the parser
// rejects `key->valuestring == NULL`, which is what cJSON gives for a number, bool or null) whose
// byte length is inside the strlen() bounds the parser enforces (sta_pass 8..64, ble_pass 4..16, ...).
// A required key that is present but too short fails the parse exactly like a missing one.
// `placeholder` allows SECRET_PLACEHOLDER as a value: /store_config swaps the stored secret back in
// before it parses, so it never reaches the length check. The factory default gets no such pass.
function assertRequiredValid(obj, what, { placeholder = false } = {}) {
    for (const k of PARSER.required) {
        assert.ok(k in obj, `${what} is missing required key "${k}"`);
        assert.equal(typeof obj[k], 'string', `${what}: required key "${k}" must be a JSON string`);
        const b = PARSER.bounds.get(k);
        if (!b || (placeholder && obj[k] === SECRET_PLACEHOLDER)) continue;
        const len = Buffer.byteLength(obj[k], 'utf8');
        assert.ok(len >= b.min && len <= b.max,
            `${what}: "${k}" is ${len} bytes, the parser accepts ${b.min}..${b.max}`);
    }
    // The one conditional key: an enabled custom AP SSID must be a length the parser accepts.
    if (obj.ap_ssid_en === 'enable') {
        const len = typeof obj.ap_ssid === 'string' ? Buffer.byteLength(obj.ap_ssid, 'utf8') : -1;
        assert.ok(len >= AP_SSID_MIN && len <= AP_SSID_MAX,
            `${what}: ap_ssid_en is "enable" but ap_ssid is not ${AP_SSID_MIN}..${AP_SSID_MAX} bytes`);
    }
}

// ---------------------------------------------------------------------------------------------
// The extraction itself. Without these the tests below could pass by checking empty sets.

test('the parser contract extracts to a plausible size', () => {
    // 11 required and 40 read today. A refactor that defeats the patterns in config_source.mjs
    // lands far below these floors.
    assert.ok(PARSER.required.size >= 10, `only ${PARSER.required.size} required keys found: ${[...PARSER.required]}`);
    assert.ok(PARSER.reads.size >= 30, `only ${PARSER.reads.size} parsed keys found`);
    // Spot-check the classification in both directions, so a regex that labels everything
    // "required" (or nothing) is caught.
    for (const k of ['wifi_mode', 'sta_ssid', 'sta_pass', 'ap_pass', 'ble_pass', 'ble_status']) {
        assert.ok(PARSER.required.has(k), `"${k}" should be classified required`);
    }
    for (const k of ['ble_power', 'engine_volt', 'timezone', 'csv_log', 'can_wake']) {
        assert.ok(PARSER.optional.has(k), `"${k}" is migration-safe and should be classified optional`);
    }
    // 4 length-checked required keys today. Without them the length half of assertRequiredValid()
    // checks nothing.
    assert.ok(PARSER.bounds.size >= 4, `only ${PARSER.bounds.size} length-bounded required keys found`);
    assert.deepEqual(PARSER.bounds.get('sta_pass'), { min: 8, max: 64 }, 'sta_pass bounds misread');
    assert.deepEqual(PARSER.bounds.get('ble_pass'), { min: 4, max: 16 }, 'ble_pass bounds misread');
    assert.ok(AP_SSID_MIN >= 1 && AP_SSID_MAX >= AP_SSID_MIN, 'AP_SSID_MIN_LEN/MAX_LEN misread');
});

test('the only conditionally-failing key is ap_ssid', () => {
    // A key whose handling can `goto config_error` without that being "the key is absent". ap_ssid
    // fails only when ap_ssid_en is "enable" and the SSID is not 3..32 characters. If this list
    // grows, someone added a new way for a stored config to fail the boot parse: decide whether
    // Submit and the factory default satisfy it, then add it here.
    assert.deepEqual([...PARSER.conditional], ['ap_ssid']);
});

// ---------------------------------------------------------------------------------------------
// The factory default.

test('the factory default parses as JSON and carries every required key with a valid value', () => {
    let def;
    assert.doesNotThrow(() => { def = JSON.parse(DEFAULT_TEXT); },
        'device_config_default[] is not valid JSON: the restore would write a file that cannot boot');
    assert.ok(Object.keys(def).length >= 20, 'factory default extracted suspiciously small');
    assertRequiredValid(def, 'device_config_default[] (else: factory-reset on every boot)');
});

test('every key in the factory default is one the parser reads', () => {
    const def = JSON.parse(DEFAULT_TEXT);
    const dead = Object.keys(def).filter(k => !PARSER.reads.has(k));
    assert.deepEqual(dead, [], `factory default writes keys nothing parses: ${dead}`);
    const retired = Object.keys(def).filter(isRetired);
    assert.deepEqual(retired, [], `factory default writes retired keys: ${retired}`);
});

test('the parser reads none of the retired keys', () => {
    const revived = [...PARSER.reads].filter(isRetired);
    assert.deepEqual(revived, [],
        `config_server_parse_cfg_into() looks up retired keys ${revived}; an old config.json carries ` +
        `them with old values, so reading them again revives dead behaviour or fails the boot parse`);
});

// ---------------------------------------------------------------------------------------------
// The page: Submit and config load, driven through the real main.js against the real page's ids.

// Load main.js with the network replaced by a recorder. Nothing answers: a test that wants a reply
// (the /load_config path) calls the recorded request's onload itself.
function loadPage() {
    const { context } = loadMainJs({ strictDom: true });
    const requests = [];
    context.XMLHttpRequest = class {
        constructor() { this.headers = {}; requests.push(this); }
        open(method, url) { this.method = method; this.url = url; }
        setRequestHeader(k, v) { this.headers[k] = v; }
        send(body) { this.body = body; this.sent = true; }
    };
    // Pending forever, never rejecting: Load() starts status polls that use fetch.
    context.fetch = () => new Promise(() => {});
    // Load() starts repeating polls; none of them is under test.
    context.setInterval = () => 0;
    // postConfig() waits out two 1 s pauses. Run them at 0 ms, and keep them ref'd: the sandbox's
    // strictDom setTimeout is unref'd, and an awaited unref'd timer can let node exit mid-test.
    context.setTimeout = (fn, _ms, ...a) => setTimeout(fn, 0, ...a);
    return { context, doc: context.document, requests };
}

async function submit(context, requests) {
    const before = requests.length;
    await context.postConfig();
    const posts = requests.slice(before).filter(r => r.method === 'POST' && r.url === '/store_config');
    assert.equal(posts.length, 1, 'postConfig() did not POST /store_config exactly once');
    assert.ok(posts[0].sent, 'postConfig() opened /store_config but never sent it');
    return JSON.parse(posts[0].body);
}

// A settings form the firmware accepts. `on` is "enable" or "disable" for every toggle at once, so
// the two Submit cases below catch a postConfig() that hard-codes a toggle instead of reading it.
function fillValid(doc, on) {
    const set = (id, v) => {
        const el = doc.getElementById(id);
        el.value = v;
        assert.equal(el.value, v, `#${id} cannot hold "${v}" (not one of its <option>s?)`);
    };
    set('wifi_mode', 'APStation');
    set('ap_ch_value', '6');
    set('ap_auto_disable', 'disable');
    set('ssid_value', 'HomeNet');
    set('pass_value', 'stationpass');
    set('sta_security', 'wpa2');
    set('ap_pass_value', 'not-the-default');
    set('ble_status', on);
    set('ble_pass_value', '482915');
    set('ble_power', '-3');
    set('sleep_status', on);
    set('can_wake', on);
    set('sleep_disable_agree', 'no');
    set('sleep_volt', '13.0');
    set('engine_volt', '13.2');
    set('sleep_time', '5');
    set('logging_master', on);
    set('csv_require_engine', on);
    doc.getElementById('csv_grid_auto').checked = true;
    doc.getElementById('led_blink').checked = on === 'enable';
}

function assertSubmitContract(body, what) {
    assertRequiredValid(body, what, { placeholder: true });
    // ap_ssid is conditional: always send it, so ap_ssid_en "enable" never arrives without it.
    assert.ok('ap_ssid' in body, `${what} omits ap_ssid`);
    const retired = Object.keys(body).filter(isRetired);
    assert.deepEqual(retired, [], `${what} writes retired keys back into config.json: ${retired}`);
    const unread = Object.keys(body).filter(k => !PARSER.reads.has(k) && !CONSUMED_ELSEWHERE.has(k));
    assert.deepEqual(unread, [], `${what} sends keys no firmware code reads: ${unread}`);
}

for (const on of ['enable', 'disable']) {
    test(`Submit (toggles ${on}d): required keys valid, no retired or unread key`, async () => {
        const { context, doc, requests } = loadPage();
        fillValid(doc, on);
        const body = await submit(context, requests);
        assertSubmitContract(body, 'the Submit body');
        // BLE is live (#159): its three settings must reach config.json as entered.
        assert.equal(body.ble_status, on);
        assert.equal(body.ble_pass, '482915');
        assert.equal(body.ble_power, '-3');
        // The toggles as entered: a hard-coded one would force sleep, CAN wake or the LED on (or
        // off) for every user on their next Submit.
        for (const k of ['sleep_status', 'can_wake', 'csv_require_engine', 'led_blink']) {
            assert.equal(body[k], on, `Submit sent ${k} "${body[k]}", the form says "${on}"`);
        }
        assert.equal(body.sta_ssid, 'HomeNet');
        assert.equal(body.sta_pass, 'stationpass');
        assert.equal(body.ap_pass, 'not-the-default');
    });
}

// A config.json written by firmware from before the #148 cleanup: the pre-cleanup factory default
// (`git show 9683f41:main/config_server.c`, device_config_default[]) with every retired key, and
// with the live values moved off their defaults so the test can tell "loaded" from "left alone".
// Live secrets are the placeholder, as /load_config serves them. batt_alert_pass and batt_mqtt_pass
// are kept in clear on purpose although current /load_config deletes them: a stronger-than-real
// input, so a loader that still reads them is exercised. Inlined rather than read from git at test
// time: CI checks out a shallow clone without that commit.
const PRE_CLEANUP_CONFIG = {
    wifi_mode: 'APStation', ap_ch: '11', sta_ssid: 'HomeNet', sta_pass: SECRET_PLACEHOLDER,
    sta_security: 'wpa2', ap_ssid_en: 'disable', ap_ssid: '',
    home_ssid: 'MeatPi', home_password: SECRET_PLACEHOLDER, home_security: 'wpa3', home_protocol: 'elm327',
    drive_ssid: 'MeatPi', drive_password: SECRET_PLACEHOLDER, drive_security: 'wpa3', drive_protocol: 'elm327',
    drive_connection_type: 'wifi', drive_mode_timeout: '60',
    can_datarate: '500K', can_mode: 'normal', port_type: 'tcp', port: '35000',
    ap_pass: SECRET_PLACEHOLDER, protocol: 'poll_log', ble_pass: SECRET_PLACEHOLDER,
    ble_status: 'enable', ble_power: '-3', sleep_status: 'disable', can_wake: 'disable',
    periodic_wakeup: 'disable', sleep_volt: '12.6', engine_volt: '13.4', wakeup_volt: '13.5',
    sleep_time: '12', wakeup_interval: '90', batt_alert: 'disable',
    batt_alert_ssid: 'MeatPi', batt_alert_pass: 'TomatoSauce', batt_alert_volt: '11.0', batt_alert_protocol: 'mqtt',
    batt_alert_url: 'mqtt://mqtt.eclipseprojects.io', batt_alert_port: '1883', batt_alert_topic: 'CAR1/voltage',
    batt_mqtt_user: 'meatpi', batt_mqtt_pass: 'meatpi', batt_alert_time: '1',
    csv_log: 'enable', log_filesystem: 'littlefs', log_storage: 'sdcard', log_period: '30',
    csv_grid_hz: '20', csv_require_engine: 'disable', led_blink: 'disable',
    timezone: 'PST8PDT,M3.2.0,M11.1.0', sleep_disable_agree: 'yes', debug: 'enabled',
    // Never in a shipped default, but on the #148 list.
    wakeup_time: '1',
};

test('the fixture really is a pre-cleanup config', () => {
    // Guards the test below against a fixture that quietly stops exercising what it claims to.
    for (const k of RETIRED.filter(k => k !== 'wakeup_time').concat('batt_alert', 'batt_mqtt_pass')) {
        assert.ok(k in PRE_CLEANUP_CONFIG, `fixture lost retired key ${k}`);
    }
    assertRequiredValid(PRE_CLEANUP_CONFIG, 'the pre-cleanup fixture', { placeholder: true });
});

async function loadConfig(context, requests, cfg) {
    await context.Load();
    const req = requests.find(r => r.method === 'GET' && r.url === '/load_config');
    assert.ok(req && req.sent, 'Load() did not request /load_config');
    assert.equal(typeof req.onload, 'function', 'Load() set no onload for /load_config');
    req.status = 200;
    req.responseText = JSON.stringify(cfg);
    await req.onload.call(req);
}

test('a pre-cleanup config.json loads, populates the form, and Submits clean', async () => {
    const { context, doc, requests } = loadPage();
    // A throw here is the #141-fallout shape: a deleted field still read by the loader, which
    // leaves the rest of the form unpopulated and Submit writing stock HTML defaults.
    await loadConfig(context, requests, PRE_CLEANUP_CONFIG);

    const val = (id) => doc.getElementById(id).value;
    assert.equal(val('wifi_mode'), 'APStation');
    assert.equal(val('ap_ch_value'), '11');
    assert.equal(val('ssid_value'), 'HomeNet');
    assert.equal(val('sta_security'), 'wpa2');
    assert.equal(val('ble_status'), 'enable');
    assert.equal(val('ble_pass_value'), SECRET_PLACEHOLDER);
    assert.equal(String(val('ble_power')), '-3');
    assert.equal(val('sleep_status'), 'disable');
    // sleep_disable_agree is not spot-checked: with sleep disabled, toggleSleepWarning() resets it
    // to "no" on every load, so the page never shows the stored "yes".
    assert.equal(val('can_wake'), 'disable');
    assert.equal(val('csv_log'), 'enable');
    assert.equal(val('csv_require_engine'), 'disable');
    assert.equal(val('timezone'), 'PST8PDT,M3.2.0,M11.1.0');
    assert.equal(doc.getElementById('led_blink').checked, false);
    // The BLE side effects of loading an enabled config.
    assert.equal(doc.getElementById('ble_warning_div').style.display, 'block');
    assert.equal(doc.getElementById('ble_pass_value').disabled, false);

    const body = await submit(context, requests);
    assertSubmitContract(body, 'Submit after loading a pre-cleanup config');
    // What the user did not touch goes back as it came, secrets as the placeholder (the firmware
    // restores the stored value), UI-less passthrough keys verbatim.
    assert.equal(body.ble_status, 'enable');
    assert.equal(body.ble_pass, SECRET_PLACEHOLDER);
    assert.equal(body.ble_power, '-3');
    assert.equal(body.sta_pass, SECRET_PLACEHOLDER);
    assert.equal(body.ap_ch, '11');
    assert.equal(body.log_period, '30');
    assert.equal(body.debug, 'enabled');
    assert.equal(body.timezone, 'PST8PDT,M3.2.0,M11.1.0');
    // The toggles the fixture moved off their factory values come back as loaded, not as the
    // page's stock defaults or a hard-coded value.
    assert.equal(body.sleep_status, 'disable');
    assert.equal(body.can_wake, 'disable');
    assert.equal(body.csv_log, 'enable');
    assert.equal(body.csv_require_engine, 'disable');
    assert.equal(body.led_blink, 'disable');
});

// handleBleStatus() is the only thing that unlocks the passkey field. Each case starts from the
// opposite state, so a function that stopped touching the DOM cannot pass.
for (const [status, display, disabled] of [['enable', 'block', false], ['disable', 'none', true]]) {
    test(`handleBleStatus() with BLE ${status}: warning ${display}, passkey ${disabled ? 'locked' : 'editable'}`, () => {
        const { context, doc } = loadPage();
        doc.getElementById('ble_status').value = status;
        assert.equal(doc.getElementById('ble_status').value, status);
        doc.getElementById('ble_warning_div').style.display = display === 'block' ? 'none' : 'block';
        doc.getElementById('ble_pass_value').disabled = !disabled;
        context.handleBleStatus(context.getElements());
        assert.equal(doc.getElementById('ble_warning_div').style.display, display);
        assert.equal(doc.getElementById('ble_pass_value').disabled, disabled);
    });
}
