#!/bin/sh
# Smoke test, run before every commit:
#   1. build the safe config (AddressSanitizer + UBSan) and run its CTest unit tests
#   2. run a headless dedicated server on the test map for 20 seconds
#   3. quit it cleanly, then fail if it exited early or abnormally, never
#      loaded the map, or the log has ERROR / ASSERT / sanitizer reports
#   SMOKE_SECONDS=20 SMOKE_MAP=testroom ./scripts/smoke.sh
set -eu
# An automated run: its own fs_homepath, or the +set values below (com_pipefile
# is CVAR_ARCHIVE) get written into the human's config. Must be set before
# lib.sh picks HOMEDIR up (.gitignore covers /.home-*/; the engine makes the dir)
SKAAR_DEV_HOME=${SKAAR_DEV_HOME:-"$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/.home-smoke"}
export SKAAR_DEV_HOME
. "$(dirname -- "$0")/lib.sh"

SECONDS_TO_RUN=${SMOKE_SECONDS:-20}
MAP=${SMOKE_MAP:-testroom}
PIPE=smoke.pipe

fail() {
    printf '%sSMOKE FAIL: %s%s\n' "$C_RED" "$*" "$C_OFF"
    [ -n "${LOG:-}" ] && printf 'log: %s\n' "$LOG"
    exit 1
}

mkdir -p "$LOGS"
STAMP=$(date +%Y%m%d-%H%M%S)
BUILD_LOG="$LOGS/smoke-build-$STAMP.log"

printf '[%s] building safe...\n' "$(now)"
if ! "$REPO/scripts/build.sh" safe > "$BUILD_LOG" 2>&1; then
    grep -E 'error|FAILED' "$BUILD_LOG" | head -20
    fail "safe build failed (see $BUILD_LOG)"
fi
grep -a 'WARNING' "$BUILD_LOG" | grep -v 'ld: warning' || true
rm -f "$BUILD_LOG"

# Unit tests registered with CTest (cmake/skaar/tests.cmake), under UBSan
if [ -f "$(build_dir safe)/CTestTestfile.cmake" ]; then
    printf '[%s] running unit tests...\n' "$(now)"
    TEST_LOG="$LOGS/smoke-tests-$STAMP.log"
    if ! ctest --test-dir "$(build_dir safe)" --output-on-failure > "$TEST_LOG" 2>&1; then
        tail -30 "$TEST_LOG"
        fail "unit tests failed (see $TEST_LOG)"
    fi
    if grep -q 'runtime error:' "$TEST_LOG"; then
        grep 'runtime error:' "$TEST_LOG" | head -10
        fail "unit tests triggered UBSan reports (see $TEST_LOG)"
    fi
    tail -1 "$TEST_LOG" | grep -q 'No tests were found' || grep -E 'tests passed|Total Test' "$TEST_LOG" | tail -2
fi

LOG="$LOGS/smoke-$STAMP.log"
BIN=$(server_bin safe)

export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"
export ASAN_OPTIONS="abort_on_error=0:halt_on_error=1"

printf '[%s] running dedicated server on %s for %ss...\n' "$(now)" "$MAP" "$SECONDS_TO_RUN"
rm -f "$HOMEDIR/base/$PIPE"
# shellcheck disable=SC2046
"$BIN" $(dev_engine_args safe) +set dedicated 1 +set developer 1 +set dev_errorDialog 0 \
    +set com_pipefile "$PIPE" +set net_ip localhost +set net_port 27999 \
    +map "$MAP" > "$LOG" 2>&1 &
PID=$!

i=0
while [ "$i" -lt "$SECONDS_TO_RUN" ]; do
    sleep 1
    kill -0 "$PID" 2>/dev/null || break
    i=$((i + 1))
done

ALIVE=1
kill -0 "$PID" 2>/dev/null || ALIVE=0

if [ "$ALIVE" = 1 ]; then
    pipe_write "$HOMEDIR/base/$PIPE" quit || kill -TERM "$PID"
    i=0
    while kill -0 "$PID" 2>/dev/null && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    kill -0 "$PID" 2>/dev/null && { kill -KILL "$PID"; fail "server didn't quit within 5s"; }
fi

STATUS=0
wait "$PID" || STATUS=$?

PROBLEMS=$(grep -a -E 'ERROR|^Error:|ASSERT|Sanitizer|runtime error:|FATAL' "$LOG" || true)

[ "$ALIVE" = 1 ] || fail "server exited after ${i}s (status $STATUS)"
[ "$STATUS" = 0 ] || fail "server exit status $STATUS"
grep -q "dev: map loaded: $MAP" "$LOG" || fail "map $MAP never loaded"
if [ -n "$PROBLEMS" ]; then
    printf '%s\n' "$PROBLEMS" | head -20
    fail "log has errors"
fi

printf '%sSMOKE PASS%s: %s ran %ss on %s, clean log (%s)\n' "$C_GRN" "$C_OFF" "$(basename "$BIN")" "$SECONDS_TO_RUN" "$MAP" "$LOG"
