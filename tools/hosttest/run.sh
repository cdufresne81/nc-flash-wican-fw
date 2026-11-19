#!/usr/bin/env bash
# Build and run the C host tests.
#
# Covers the decision logic that used to be unreachable from any test: the CSV
# datalogger's RTC crash-guard bring-up chain, the manual-override lifetime and the
# logging gate, plus poll_log's recording gate (does the ENGINE actually turn), the
# ECU-flash fence, when the default sensor list may replace auto_pid.json, and the
# NC Flash logging session's lease and token rules, and the rule that benches the logger
# while an ELM327 app is using the car.
# Those live in the *_logic.c files compiled below, deliberately free of ESP-IDF includes
# so a stock compiler can build them.
#
# The one exception is the auto_pid.json parser (components/autopid/autopid_config.c),
# the source of poll_log's whole PID table: it is compiled UNMODIFIED against the stub
# ESP-IDF headers in tools/hosttest/stubs/ (stdlib allocator, silent logging, a
# FS_MOUNT_POINT the test points at a temp dir) and real cJSON, under
# -fsanitize=address,undefined,float-cast-overflow so a leak, overrun or UB in the parser
# fails the run. The test carries a verbatim copy of autopid.c's get_pid_from_string();
# this script diffs the two and fails if they drift.
# cJSON is found, in order, at:
#   $CJSON_DIR                        a cJSON source checkout (CI fetches the exact commit
#                                     ESP-IDF v5.5.3 pins -- see the workflow)
#   $IDF_PATH/components/json/cJSON   the copy the firmware itself links
#   pkg-config libcjson               a system package (e.g. apt install libcjson-dev)
# and the script FAILS, never skips, when none is there.
#
# Seconds to run, no toolchain beyond cc. Wired into the "checks" job in
# .github/workflows/build-firmware.yml next to the JS host tests.
#
# Usage: bash tools/hosttest/run.sh
#        IDF_PATH=~/esp/esp-idf bash tools/hosttest/run.sh   (if IDF is not exported)

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

"$CC" -std=c11 -Wall -Wextra -Werror -O1 \
    -I "$REPO/main" \
    "$REPO/tools/hosttest/ext_client_test.c" \
    "$REPO/main/ext_client_logic.c" \
    -o "$OUT/ext_client_test"

"$OUT/ext_client_test"

# ---- auto_pid.json parser (real autopid_config.c + stubs + cJSON) ----------------------
# stubs/ precedes main/ so its hw_config.h shadows the real one (FS_MOUNT_POINT), while
# obd2_standard_pids.h still comes from main/.
# Sanitizers are the leak/overrun half of this test. CI sets HOSTTEST_REQUIRE_SANITIZERS=1
# so a runner image that loses them fails the job; locally (e.g. a clang without its ASan
# runtime) the test still runs, without them, and says so.
# float-cast-overflow is NOT part of GCC's "undefined" group; it is named because the parser
# clamps doubles before casting them to uint8_t (SampleEvery) and that clamp is what it pins.
SAN=(-fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer)
if ! echo 'int main(void){return 0;}' | "$CC" -x c - "${SAN[@]}" -o "$OUT/san_probe" 2>/dev/null; then
    if [ "${HOSTTEST_REQUIRE_SANITIZERS:-0}" = 1 ]; then
        echo "run.sh: $CC cannot build with ${SAN[*]} and HOSTTEST_REQUIRE_SANITIZERS=1" >&2
        exit 1
    fi
    echo "run.sh: NOTE $CC cannot link ${SAN[*]}; autopid_config runs WITHOUT leak/overrun/UB checks" >&2
    SAN=()
fi
CJSON_INC=()
CJSON_SRC=()
CJSON_LIBS=()
if [ -n "${CJSON_DIR:-}" ]; then
    if [ ! -f "$CJSON_DIR/cJSON.c" ] || [ ! -f "$CJSON_DIR/cJSON.h" ]; then
        echo "run.sh: CJSON_DIR=$CJSON_DIR holds no cJSON.c/cJSON.h" >&2
        exit 1
    fi
    CJSON_INC=(-I "$CJSON_DIR"); CJSON_SRC=("$CJSON_DIR/cJSON.c")
elif [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/components/json/cJSON/cJSON.c" ]; then
    CJSON_INC=(-I "$IDF_PATH/components/json/cJSON"); CJSON_SRC=("$IDF_PATH/components/json/cJSON/cJSON.c")
elif command -v pkg-config >/dev/null && pkg-config --exists libcjson; then
    read -r -a CJSON_INC <<< "$(pkg-config --cflags libcjson)"
    read -r -a CJSON_LIBS <<< "$(pkg-config --libs libcjson)"
else
    echo "run.sh: cJSON not found -- the autopid_config test needs it. Set CJSON_DIR to a" >&2
    echo "        cJSON checkout, export IDF_PATH, or install libcjson-dev." >&2
    exit 1
fi
if [ "${#CJSON_SRC[@]}" -gt 0 ]; then
    # Third-party source: instrumented like the parser, but not held to -Werror.
    "$CC" -std=c11 -O1 -g "${SAN[@]}" "${CJSON_INC[@]}" -c "${CJSON_SRC[0]}" -o "$OUT/cJSON.o"
    CJSON_LIBS=("$OUT/cJSON.o")
fi

# The test's get_pid_from_string() must stay a verbatim copy of autopid.c's (the test
# cannot link autopid.c: it drags in the whole autopid runtime). Compare the two function
# bodies; an empty extraction means the signature moved, which fails too.
extract_gpfs() { awk '/^const std_pid_t \*get_pid_from_string\(/,/^}/' "$1"; }
GPFS_REAL="$(extract_gpfs "$REPO/components/autopid/autopid.c")"
GPFS_COPY="$(extract_gpfs "$REPO/tools/hosttest/autopid_config_test.c")"
if [ "$(printf '%s\n' "$GPFS_REAL" | wc -l)" -lt 10 ] || [ "$GPFS_REAL" != "$GPFS_COPY" ]; then
    echo "run.sh: get_pid_from_string() in tools/hosttest/autopid_config_test.c is not a verbatim" >&2
    echo "        copy of components/autopid/autopid.c's -- paste the current one into the test." >&2
    diff <(printf '%s\n' "$GPFS_REAL") <(printf '%s\n' "$GPFS_COPY") >&2 || true
    exit 1
fi

# -Wno-unused-result: glibc marks fread() warn_unused_result under _FORTIFY_SOURCE, and
# parse_json_file() ignores it (newlib on the device does not warn). Everything else in
# the parser is held to -Werror like the rest of this script.
"$CC" -std=c11 -Wall -Wextra -Werror -Wno-unused-result -O1 -g "${SAN[@]}" \
    -I "$REPO/tools/hosttest/stubs" \
    -I "$REPO/components/autopid" \
    -I "$REPO/main" \
    "${CJSON_INC[@]}" \
    "$REPO/tools/hosttest/autopid_config_test.c" \
    "$REPO/components/autopid/autopid_config.c" \
    "${CJSON_LIBS[@]}" \
    -o "$OUT/autopid_config_test"

"$OUT/autopid_config_test" "$REPO/tools/hosttest/fixtures" "$REPO/main/default_auto_pid.json"
