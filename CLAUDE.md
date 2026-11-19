# Working in this repo

ESP32-S3 firmware (ESP-IDF v5.5.3, target esp32s3) for the WiCAN PRO, forked into an NC Miata
datalogger. `docs/internals/README.md` indexes the design docs; read the relevant one before
changing a subsystem.

## Every change ships with a test

A PR that changes behaviour, or deletes or refactors code on a live path, adds or extends a test
in the same PR. Say in the PR description which test covers the change. If a change cannot be
tested off the device, say so plainly and list what to check on the bench.

Where tests go (all run in the "Static checks" job of `.github/workflows/build-firmware.yml`):

- **C logic**: put decisions in a `*_logic.c` file with no ESP-IDF includes and pin it with a
  test in `tools/hosttest/`, wired into `tools/hosttest/run.sh`. Code that cannot be made pure
  can still be compiled on the host against `tools/hosttest/stubs/`, as
  `autopid_config_test.c` does for the real `auto_pid.json` parser.
- **Web UI** (`main/web/src/main.js`): `tools/webtest/*.test.mjs`, see `tools/webtest/README.md`.
- **Config keys**: `tools/webtest/config_contract.test.mjs` pins that the parser's required keys,
  the factory default and what Submit sends agree. Adding or retiring a `config.json` key means
  updating that test.

A test must fail when the thing it pins is broken. Prove it once by breaking the code, and do
not weaken, skip or delete a test to get green.

## Checks before pushing

```bash
bash tools/hosttest/run.sh              # needs cJSON: export IDF_PATH or set CJSON_DIR
node --test tools/webtest/*.test.mjs
python3 tools/lint_web.py
python3 tools/check_docs.py
```

- After editing `main/web/homepage_full.html`, run `python3 tools/build_web.py` and commit the
  regenerated `main/web/src/homepage.html`.
- The firmware build rewrites `sdkconfig`; run `git checkout sdkconfig` afterwards.
- `python3 tools/check_build_budget.py --log <build log>` gates warnings and image size against
  `tools/build_baseline.json`. The warning count may not rise.

## Things that bite

- `components/fast_log/` holds the live `poll_log.c`. The name is historical.
- An old `config.json` or `auto_pid.json` on a device must keep loading: retired keys are
  ignored, never required, never an error.
- Nothing is verified on hardware unless a person ran it on the bench. Say so in the PR.
