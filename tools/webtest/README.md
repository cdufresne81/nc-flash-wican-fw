# Web UI unit tests

```sh
node --test tools/webtest/*.test.mjs
```

No dependencies, no install step, no browser — Node's built-in test runner
against `main/web/src/main.js` evaluated in a `vm` sandbox. Node 18+.

> On Windows, pass the glob (`tools/webtest/*.test.mjs`), not the directory —
> `node --test tools/webtest/` tries to resolve the path as a module and fails.

## What these cover, and why these functions

`main.js` contains several pairs of functions that **must be exact inverses**:

| Pair | Breaks as |
|---|---|
| `parsePidText` / `composePidText` | a stored PID stops round-tripping; the device polls different bytes than the row shows |
| `abcToBn` / `bnToAbc` | a load/save cycle silently rewrites an expression; the channel logs a plausible wrong number forever |
| `rmbaSplitIdent` / `rmbaJoinIdent` | a mode-23 address/size no longer reassembles to the stored string |
| `rangeToArith` (idempotence) | a config keeps mutating on every save instead of settling |

None of these fail loudly. The page renders, the device polls, the CSV fills —
and the damage appears as a changed `auto_pid.json` or a wrong column days
later. That is exactly what a test pins and a human reviewer does not.

The suite also pins facts that were previously *wrong in shipped code*, so a
regression is caught by name rather than rediscovered on the bench:

- `PID_TEXT_MAX` is **derived** (15), not the hardcoded `10` that silently
  rejected every mode-23 row on Store before issue #51.
- `exprDataOffset` comes from **echo** bytes, not identifier length. Mode 23
  sends a 6-byte operand and echoes none, so its data starts at `B2`; the old
  derivation computed `B8` and read past the frame.
- An out-of-range mode-23 size is *non-canonical*, not an exception — it used
  to block every save of the whole page, not just its own row.
- `Fn` earns a friendly letter only when all four of its bytes sit inside the
  single 8-byte frame `poll_log` evaluates.

## The config.json contract (`config_contract.test.mjs`)

Three places must agree on config.json's keys: the firmware parser
`config_server_parse_cfg_into()`, the factory default `device_config_default[]`
(both in `main/config_server.c`), and the Submit body `postConfig()` builds.
Get one wrong and:

| Disagreement | Breaks as |
|---|---|
| factory default lacks a required key | the boot-time restore writes a file that fails the same parse: factory reset on **every** boot |
| Submit omits a required key | `/store_config` refuses every Submit with HTTP 400: no setting can be saved |
| parser reads a retired key again | an old config.json still carries it with an old value, which then revives dead behaviour or fails the parse |
| default or Submit writes a retired or unread key | dead settings creep back into config.json |

`config_source.mjs` reads the contract out of the C source: it joins line
splices, strips comments (two dead defaults sit in `//` comments under the live
one), evaluates the default's string literals and `SYNC_SYS_TIME_DEFAULT_TZ`,
and classifies each `cJSON_GetObjectItem(root, "K")` lookup:

- **required**: the statement right after the lookup is an `if` with
  `key == 0` / `key == NULL` / `!key` as one of its top-level `||` terms, and
  its body does `goto config_error`;
- **conditional**: the key's handling can `goto config_error`, just not because
  it is missing. Today only `ap_ssid` does this (it fails only when `ap_ssid_en`
  is "enable"), and a test pins that list so a new case has to be looked at;
- **optional**: no `goto config_error`, so the key is migration-safe.

Required keys also carry the length limits their `goto config_error` conditions
put on `strlen(key->valuestring)` (`sta_ssid` 1..32, `sta_pass` and `ap_pass`
8..64, `ble_pass` 4..16). The factory default, the Submit body and the fixture
are checked against them, not just for presence: a 7-character `ap_pass` in the
default fails the parse exactly like a missing one. Submit may carry the secret
placeholder instead, because `/store_config` restores the stored value first.

Every lookup on `root` must be in the `var = cJSON_GetObjectItem(root, "K");`
form. `parserKeys()` counts all `cJSON_GetObjectItem*`/`cJSON_HasObjectItem`
calls on `root` and throws if any is in another shape, because an unrecognised
lookup would be invisible to every test here.

The first test asserts minimum counts and a few known classifications. If a
refactor defeats the patterns, that test fails by name. Without it, the other
tests would pass while checking empty sets.

The page-side tests drive the real `postConfig()` and `Load()` against a
`strictDom` page, with `XMLHttpRequest` swapped for a recorder. The old-config
test loads a pre-#148 config.json (inlined, because CI's shallow clone has no
history to `git show` from) and checks the form, the BLE fields and the next
Submit. The Submit test runs twice, with every toggle enabled and then
disabled, so a `postConfig()` that hard-codes a toggle fails one of them. Under
`strictDom`, `<select>`s carry their real `<option>`s and keep
`value`/`selectedIndex` in step, and `querySelector('#id' | '.class')`
resolves against the page (`.class` only to elements that have an id, which is
exact for the one class `Load()` queries). `Load()` needs both.

## Adding a test

1. Add the symbol to `EXPORTS` in `sandbox.mjs`.
2. `requireExports('yourFn')` in a `*.test.mjs` file.

If a symbol disappears from `main.js`, `requireExports` fails with a message
naming it and telling you whether to update the harness or the test — rather
than the tests silently passing because the function is `undefined`.

**Cross-realm gotcha:** values built inside the sandbox carry that realm's
`Object`/`Array` prototypes, so `assert.deepStrictEqual` rejects them as *"same
structure but not reference-equal"*. Normalise first —
`JSON.parse(JSON.stringify(v))`, spelled `plain()` in the existing tests.

## Not covered yet

`emitSensorsYaml()` / `parseSensorsYaml()` (issue #63) are the third
exact-inverse pair and are **not** tested here. `emitSensorsYaml` renders
`buildAutoTableJson()`, which reads real `.pid-entry` DOM nodes, so covering it
needs a DOM stub rich enough to hold a populated table rather than the minimal
load-survival stub in `sandbox.mjs`. Worth doing — the import path already
shipped two bugs that this would have caught (the export/import asymmetry on a
non-canonical mode-23 row, and the expression-vs-read-size mismatch).

## Related

`tools/lint_web.py` is the other web guard — it checks that every
`getElementById` literal resolves, every inline handler exists, the generated
`src/homepage.html` is current, and every `elements.X` read is a key
`getElements()` returns. Different failure class: lint catches *dangling
references*, these tests catch *wrong answers*. Run both.

`loadMainJs({ strictDom: true })` makes `getElementById` return `null` for any
id the real page does not define, instead of the default stub-for-everything.
`submit_enable.test.mjs` and `config_contract.test.mjs` use it. The
`submit_enable` pair is the regression guard for the dead "Submit changes" button, where a deleted DOM element left `submit_enable()`
throwing on every keystroke so no setting on the page could be saved.
