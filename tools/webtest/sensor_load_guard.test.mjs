// Regression test for issue #155: Settings Submit overwrote the stored sensor list with an
// empty one.
//
// When GET /load_auto_pid failed (Wi-Fi drop, a 500 from a failed malloc, a half-way
// loadAutoTable throw), the Logger tables stayed empty and the skip-unchanged baseline
// stayed null. postConfig() -> storeAutoTableData(true) compares against that baseline, a
// null never matches, so it ALWAYS posted the empty table to /store_auto_data. The user's
// sensors were gone, and after #154 the boot seed would not put them back (the empty file
// carries `calculated`, so it counts as a list saved on purpose).

import test from 'node:test';
import assert from 'node:assert/strict';
import { loadMainJs } from './sandbox.mjs';

// One page load: main.js in a sandbox, a fake XHR for /load_auto_pid, and a fetch that
// records every URL it is asked for. XHR POSTs (the settings themselves) land in `posts` too.
function page() {
    const { context } = loadMainJs();
    const xhrs = [];
    context.XMLHttpRequest = class {
        constructor() { this.status = 0; this.responseText = ''; xhrs.push(this); }
        open(method, url) { this.method = method; this.url = url; }
        setRequestHeader() {}
        send() { if (this.method === 'POST') posts.push(this.url.replace(/^\//, '')); }
    };
    const posts = [];
    context.fetch = async (url) => {
        posts.push(String(url).replace(/^\//, ''));
        return { ok: true, status: 200, text: async () => '{}', json: async () => ({}) };
    };
    const loadReply = (status, body) => {
        context.loadautoPID();
        const x = xhrs.find(r => r.url === '/load_auto_pid');
        x.status = status;
        x.responseText = body;
        x.onload();
    };
    const loadError = () => {
        context.loadautoPID();
        xhrs.find(r => r.url === '/load_auto_pid').onerror(new Error('net'));
    };
    return { context, posts, loadReply, loadError, stores: () => posts.filter(u => u === 'store_auto_data') };
}

test('a network error on /load_auto_pid: Submit does not post the sensor table', async () => {
    const p = page();
    p.loadError();
    assert.equal(p.context.sensorsLoadState, 'failed');
    assert.equal(await p.context.storeAutoTableData(true), true, 'Submit must still save the other settings');
    assert.deepEqual(p.stores(), []);
});

test('a 500 HTML body on /load_auto_pid: Submit does not post the sensor table', async () => {
    const p = page();
    p.loadReply(500, '<html>500 Internal Server Error</html>');
    assert.equal(p.context.sensorsLoadState, 'failed');
    await p.context.storeAutoTableData(true);
    assert.deepEqual(p.stores(), []);
});

test('a 200 reply that is not JSON: Submit does not post the sensor table', async () => {
    const p = page();
    p.loadReply(200, 'garbage');
    assert.equal(p.context.sensorsLoadState, 'failed');
    await p.context.storeAutoTableData(true);
    assert.deepEqual(p.stores(), []);
});

test('Submit before /load_auto_pid returns does not post the sensor table', async () => {
    const p = page();
    assert.equal(p.context.sensorsLoadState, 'pending');
    assert.equal(await p.context.storeAutoTableData(true), true);
    assert.deepEqual(p.stores(), []);
});

test('the Logger Store button refuses after a failed load', async () => {
    const p = page();
    p.loadError();
    assert.equal(await p.context.storeAutoTableData(), false);
    assert.deepEqual(p.stores(), []);
});

test('the whole Submit path (postConfig) posts settings but not the table after a failed load', async () => {
    const p = page();
    p.loadReply(500, '<html>500</html>');
    await p.context.postConfig();   // two 1 s waits inside
    assert.deepEqual(p.stores(), [], 'no POST to /store_auto_data');
    assert.ok(p.posts.length > 0, 'the settings themselves must still be posted');
});

test('after a good load, Store posts and an unchanged Submit skips', async () => {
    const p = page();
    p.loadReply(200, JSON.stringify({ pids: [], can_filters: [], calculated: [] }));
    assert.equal(p.context.sensorsLoadState, 'loaded');
    await p.context.storeAutoTableData(true);
    assert.deepEqual(p.stores(), [], 'unchanged table: Submit skips');
    await p.context.storeAutoTableData();
    assert.deepEqual(p.stores(), ['store_auto_data'], 'Store always posts');
});

test('no stored file (NONE): an unchanged Submit does not create one, Store may', async () => {
    const p = page();
    p.loadReply(200, 'NONE');
    assert.equal(p.context.sensorsLoadState, 'none');
    await p.context.storeAutoTableData(true);
    assert.deepEqual(p.stores(), [], 'an empty file would block the #154 boot seed');
    await p.context.storeAutoTableData();
    assert.deepEqual(p.stores(), ['store_auto_data']);
});
