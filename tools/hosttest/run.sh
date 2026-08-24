#!/usr/bin/env bash
# Build and run the C host tests.
#
# Covers the decision logic that used to be unreachable from any test: the CSV
# datalogger's RTC crash-guard bring-up chain, the manual-override lifetime and the
# logging gate, plus poll_log's recording gate (does the ENGINE actually turn).
# Everything under test lives in components/csv_logger/csv_bringup_logic.c and
# components/fast_log/poll_gate_logic.c, both deliberately free of ESP-IDF includes so a
# stock compiler can build them.
#
# Seconds to run, no toolchain beyond cc. Wired into the "checks" job in
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
