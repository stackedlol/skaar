#!/bin/sh
# Soak test: a long scripted session that cycles every trainer scenario, the
# movement map on both rulesets (incl. the jump pad), the test room and the
# arena (firing, switching weapons), while sampling memory and frame times.
#   SOAK_MINUTES=120 ./scripts/soak.sh [safe|fast]     (defaults: safe, 120 min)
# Fails if the game crashes or exits early, the log has ERROR / ASSERT /
# sanitizer reports, or resident memory keeps growing (> 25% and > 64 MB from
# the first to the last tenth of the run). Samples go to logs/soak-*.csv.
# Runs in its own home (.home-soak) as an agent run: background window, no
# mouse capture, no network sockets.
set -eu
. "$(dirname -- "$0")/lib.sh"

CONFIG=${1:-safe}
MINUTES=${SOAK_MINUTES:-120}
H="$REPO/.home-soak"
STAMP=$(date +%Y%m%d-%H%M%S)
CSV="$LOGS/soak-$STAMP.csv"
STEP_SECONDS=${SOAK_STEP_SECONDS:-40}

fail() { printf '%sSOAK FAIL: %s%s\n' "$C_RED" "$*" "$C_OFF"; stop_game; exit 1; }

rm -rf "$H"
mkdir -p "$H/base" "$LOGS"
QLOG="$H/base/qconsole.log"
PIPE="$H/base/dev.pipe"

printf '[%s] building %s...\n' "$(now)" "$CONFIG"
"$REPO/scripts/build.sh" "$CONFIG" > "$LOGS/soak-build-$STAMP.log" 2>&1 || { tail -20 "$LOGS/soak-build-$STAMP.log"; exit 1; }

game_pid() { pgrep -f "fs_homepath $H" | head -1; }
# never blocks: a game that died leaves its pipe behind, and a plain redirect
# into a reader-less FIFO would hang the soak forever (sample() reports it)
send() { pipe_write "$PIPE" "$*" || true; }
stop_game() {
    send quit 2>/dev/null || true
    sleep 2
    p=$(game_pid || true)
    [ -n "$p" ] && kill -TERM "$p" 2>/dev/null || true
}

# UBSan keeps going and reports; ASan (when the toolchain allows) stops on the first error
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"
SKAAR_DEV_HOME="$H" DEV_DEBUGGER=none "$REPO/scripts/dev.sh" "$CONFIG" --menu \
    +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 960 +set r_customheight 540 \
    +set com_maxfps 240 +set dev_toasts 0 > /dev/null 2>&1 &

n=0
while [ ! -p "$PIPE" ] && [ "$n" -lt 120 ]; do sleep 0.5; n=$((n + 1)); done
[ -p "$PIPE" ] || fail "the game didn't start"
PID=$(game_pid)

echo "time_s,step,rss_kb,fps,frame_p99_ms" > "$CSV"
START=$(date +%s)
END=$((START + MINUTES * 60))

sample() {
    # liveness first: a crash or quit leaves nothing to write the pipe to
    rss=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ')
    [ -n "$rss" ] || fail "game exited during step '$1'"
    send dev_stats
    sleep 0.5
    stats=$(grep -a "dev_stats:" "$QLOG" | tail -1)
    fps=$(printf '%s' "$stats" | sed -n 's/.*fps \([0-9.]*\),.*/\1/p')
    p99=$(printf '%s' "$stats" | sed -n 's/.*p99 \([0-9.]*\) ms.*/\1/p')
    echo "$(( $(date +%s) - START )),$1,$rss,${fps:-0},${p99:-0}" >> "$CSV"
}

# hold a key (console command pair) for a while
hold() { send "+$1"; sleep "$2"; send "-$1"; }

step_run() {
    name=$1
    t_end=$(( $(date +%s) + STEP_SECONDS ))
    while [ "$(date +%s)" -lt "$t_end" ]; do
        case "$name" in
            arena) send "weapon $(( (RANDOM_N % 4) == 0 ? 3 : (RANDOM_N % 4) + 4 ))"; hold attack 1; hold forward 0.5; hold moveup 0.2 ;;
            movement*) hold forward 0.8; hold moveup 0.2; hold moveright 0.6; send "setviewpos 2048 576 70 0"; sleep 1.5 ;;
            *) hold forward 0.5; hold back 0.5; sleep 1 ;;
        esac
        RANDOM_N=$(( (RANDOM_N * 1103515245 + 12345) % 2147483648 ))
        sample "$name"
    done
}
RANDOM_N=7

SCENARIOS=$(ls "$REPO/data/base/scenarios" | sed -n 's/\.json$//p')

printf '[%s] soaking for %s min (pid %s), samples in %s\n' "$(now)" "$MINUTES" "$PID" "$CSV"
cycle=0
while [ "$(date +%s)" -lt "$END" ]; do
    cycle=$((cycle + 1))
    send "g_movement vq3"; send "devmap testroom"; sleep 3; step_run testroom
    for s in $SCENARIOS; do
        [ "$(date +%s)" -lt "$END" ] || break
        send "devmap range"; sleep 3; send "sk_start $s"; step_run "scenario:$s"
    done
    [ "$(date +%s)" -lt "$END" ] || break
    send "g_movement cpm"; send "devmap movement"; sleep 3; step_run movement-cpm
    send "g_movement vq3"; send "devmap movement"; sleep 3; step_run movement-vq3
    send "devmap duel"; sleep 3; step_run arena
    printf '[%s] cycle %d done, rss %s KB\n' "$(now)" "$cycle" "$(tail -1 "$CSV" | cut -d, -f3)"
done

stop_game

# ---- verdict ----------------------------------------------------------------
PROBLEMS=$(grep -a -E 'ERROR|^Error:|ASSERT|Sanitizer|runtime error:|crash report' "$QLOG" | grep -v 'dev_crash' | head -20 || true)
[ -z "$PROBLEMS" ] || { printf '%s\n' "$PROBLEMS"; fail "log has errors ($QLOG)"; }

rc=0
awk -F, 'NR > 1 { rss[n++] = $3; p99sum += $5; if ($5 > p99max) p99max = $5 }
END {
    if (n < 10) { print "too few samples: " n; exit 2 }
    k = int(n / 10); a = 0; b = 0
    for (i = 0; i < k; i++) { a += rss[i]; b += rss[n - 1 - i] }
    a /= k; b /= k
    growth = (b - a) / a
    printf "samples %d, rss first tenth %.0f MB, last tenth %.0f MB (%+.1f%%), frame p99 mean %.2f ms, max %.2f ms\n",
        n, a / 1024, b / 1024, growth * 100, p99sum / n, p99max
    if (growth > 0.25 && (b - a) > 65536) exit 1
}' "$CSV" || rc=$?
# 2 is awk's "too few samples", not a leak: don't diagnose a run that measured nothing
if [ "$rc" = 2 ]; then
    fail "too few samples in $CSV (the run was too short to judge memory)"
fi
[ "$rc" = 0 ] || fail "resident memory kept growing (see $CSV)"

printf '%sSOAK PASS%s: %s min, %d cycles, no errors (%s)\n' "$C_GRN" "$C_OFF" "$MINUTES" "$cycle" "$CSV"
