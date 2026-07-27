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
`getElementById` literal resolves, every inline handler exists, and the
generated `src/homepage.html` is current. Different failure class: lint catches
*dangling references*, these tests catch *wrong answers*. Run both.
