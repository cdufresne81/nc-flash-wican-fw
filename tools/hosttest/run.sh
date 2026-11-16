#!/usr/bin/env bash
# Build and run the C host tests.
#
# Covers the decision logic that used to be unreachable from any test: the CSV
# datalogger's RTC crash-guard bring-up chain, the manual-override lifetime and the
# logging gate, plus poll_log's recording gate (does the ENGINE actually turn), the
# ECU-flash fence, when the default sensor list may replace auto_pid.json, and the
# NC Flash logging session's lease and token rules, and the fast flash ('K') against a fake ECU
# that implements the fast-flash kernel v3 protocol over a 1 MB model flash.
# Everything under test lives in components/csv_logger/csv_bringup_logic.c and
# components/fast_log/poll_gate_logic.c, both deliberately free of ESP-IDF includes so a
# stock compiler can build them.
#
# Seconds to run, no toolchain beyond cc and python3 (the fast-flash test data is made with
# Python's lzma by tools/hosttest/fast_flash_gen.py). Wired into the "checks" job in
# .github/workflows/build-firmware.yml next to the JS host tests.
#
# Usage: bash tools/hosttest/run.sh

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"

"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$REPO/components/csv_logger" \
    "$REPO/tools/hosttest/csv_bringup_test.c" \
    "$REPO/components/csv_logger/csv_bringup_logic.c" \
    -o "$OUT/csv_bringup_test"

"$OUT/csv_bringup_test"

"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$REPO/components/fast_log" \
    "$REPO/tools/hosttest/poll_gate_test.c" \
    "$REPO/components/fast_log/poll_gate_logic.c" \
    -o "$OUT/poll_gate_test"

"$OUT/poll_gate_test"

"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$REPO/main" \
    "$REPO/tools/hosttest/flash_fence_test.c" \
    "$REPO/main/flash_fence_logic.c" \
    -o "$OUT/flash_fence_test"

"$OUT/flash_fence_test"

"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$REPO/main" \
    "$REPO/tools/hosttest/auto_pid_seed_test.c" \
    "$REPO/main/auto_pid_seed_logic.c" \
    -o "$OUT/auto_pid_seed_test"

"$OUT/auto_pid_seed_test"

"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$REPO/main" \
    "$REPO/tools/hosttest/datalog_session_test.c" \
    "$REPO/main/datalog_session_logic.c" \
    -o "$OUT/datalog_session_test"

"$OUT/datalog_session_test"

PYTHON="${PYTHON:-python3}"
"$PYTHON" "$REPO/tools/hosttest/fast_flash_gen.py" "$OUT/ffdata"

"$CC" -std=c11 -Wall -Wextra -Werror -O1     -I "$REPO/main" -I "$REPO/tools/hosttest"     "$REPO/tools/hosttest/fast_flash_test.c"     "$REPO/tools/hosttest/fast_flash_fake_ecu.c"     "$REPO/main/fast_flash_logic.c"     -o "$OUT/fast_flash_test"

"$OUT/fast_flash_test" "$OUT/ffdata"
