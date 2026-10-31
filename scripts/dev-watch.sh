#!/bin/sh
# The game's "localhost dev server": save a file -> rebuild -> reload/relaunch,
# with errors shown in this terminal and in-game. One command, one terminal.
#   ./scripts/dev-watch.sh [fast|safe] [engine args for the game...]
#
# What a change triggers (docs/dev-loop.md):
#   code/game, code/cgame, code/q3_ui, game/  build, hot-reload modules (no relaunch)
#   data/maps-src/*.json                       build (mapc), reload the map in place
#   data/base/*                                no build, dev_reload <file> in the game
#   anything else (engine C, cmake, tools)     build, relaunch the game
# A failed build never relaunches: the last working game keeps running and the
# compiler errors are printed in red (and toasted in-game).
# DEV_WATCH_RELAUNCH=1 relaunches on every successful build instead of hot-reloading.
# Ctrl+C stops the watcher and closes the game.
set -u
. "$(dirname -- "$0")/lib.sh"

CONFIG=fast
case "${1:-}" in fast|safe) CONFIG=$1; shift ;; esac
GAME_ARGS="$*"
BIN=$(client_bin "$CONFIG")
BUILD_LOG="$LOGS/dev-watch-build.log"
ALWAYS_RELAUNCH=${DEV_WATCH_RELAUNCH:-0}

command -v fswatch >/dev/null 2>&1 || die "fswatch not found: brew install fswatch (macOS) / sudo apt install fswatch (Ubuntu)"
mkdir -p "$LOGS"

now_ms() { perl -MTime::HiRes=time -e 'printf "%d", time * 1000' 2>/dev/null || echo "$(( $(date +%s) * 1000 ))"; }
secs_since() { awk -v a="$1" -v b="$(now_ms)" 'BEGIN { printf "%.1f", (b - a) / 1000 }'; }

# One-line status: terminal + session log (so watch.sh and the log see it too)
status() {
    color=$1; shift
    line="[$(now)] $*"
    printf '%s%s%s\n' "$color" "$line" "$C_OFF"
    [ -e "$LOGS/latest.log" ] && printf '%s\n' "$line" >> "$LOGS/latest.log"
}

game_running() { pgrep -f "$BIN" >/dev/null 2>&1; }

# Build outputs newer than the last launch/reload? (a save during a build can
# queue a second, no-op build that shouldn't relaunch again)
STAMP_FILE="$LOGS/.dev-watch-stamp"
mark_outputs() { touch "$STAMP_FILE"; }
outputs_changed() {
    [ -f "$STAMP_FILE" ] || return 0
    [ -n "$(find "$(out_dir "$CONFIG")" -newer "$STAMP_FILE" \
        \( -name '*.dylib' -o -name '*.so' -o -name '*.dll' -o -name skaar -o -name skaarded -o -name '*.bsp' \) \
        -print 2>/dev/null | head -1)" ]
}

# Console text for the in-game toast: no quotes or ; (they'd split the command)
toast() { send_cmd "echo $(printf '%s' "$*" | tr -d '";' | cut -c1-160)" || true; }

stop_game() {
    game_running || return 0
    send_cmd "quit" || true
    n=0
    while game_running && [ "$n" -lt 30 ]; do sleep 0.1; n=$((n + 1)); done
    if game_running; then
        pkill -TERM -f "$BIN" 2>/dev/null
        n=0
        while game_running && [ "$n" -lt 20 ]; do sleep 0.1; n=$((n + 1)); done
        game_running && pkill -KILL -f "$BIN" 2>/dev/null
    fi
    return 0
}

start_game() {
    # dev.sh writes everything to logs/session-*.log; this terminal shows the
    # filtered view from the watcher below
    # shellcheck disable=SC2086
    "$REPO/scripts/dev.sh" "$CONFIG" $GAME_ARGS > /dev/null 2>&1 &
    # wait for the pipe so follow-up commands (dev_reload, toasts) land
    n=0
    while [ ! -p "$(pipe_path)" ] && [ "$n" -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
}

# Returns 0 on success; prints errors and toasts them on failure
build() {
    t0=$(now_ms)
    if "$REPO/scripts/build.sh" "$CONFIG" > "$BUILD_LOG" 2>&1; then
        BUILD_TIME=$(secs_since "$t0")
        return 0
    fi
    BUILD_TIME=$(secs_since "$t0")
    errors=$(grep -c -E '(^|[^a-z])error:' "$BUILD_LOG" || true)
    [ "$errors" -gt 0 ] || errors=1
    # errors with their source excerpt, without the long compiler command lines
    printf '%s' "$C_RED"
    grep -v -E '^(ccache |/usr/bin/|/Library/|/opt/|\[[0-9]+/[0-9]+\])' "$BUILD_LOG" |
        grep -E -A3 '(^|[^a-z])error:|CMake Error' | grep -v '^--$' | head -40
    printf '%s' "$C_OFF"
    [ -e "$LOGS/latest.log" ] && grep -E '(^|[^a-z])error:' "$BUILD_LOG" | head -20 >> "$LOGS/latest.log"
    status "$C_RED" "build FAILED in ${BUILD_TIME}s, $errors error(s) (full output: $BUILD_LOG)"
    toast "^1ERROR: build failed, $errors error(s): $(grep -m1 -E '(^|[^a-z])error:' "$BUILD_LOG" | sed "s|$REPO/||")"
    return 1
}

# Classify one batch of changed paths and act on it
handle_batch() {
    kind=none          # none < data < map < module < engine
    data_files=
    map_files=
    for path in $1; do
        rel=${path#"$REPO"/}
        case "$rel" in
            .git/*|build-*|logs/*|.home/*|*.swp|*~|*.DS_Store|*/.#*|*4913) continue ;;
            data/maps-src/*.json)
                map_files="$map_files maps/$(basename "$rel" .json).bsp"
                [ "$kind" = none ] || [ "$kind" = data ] && kind=map ;;
            data/*)
                data_files="$data_files $rel"
                [ "$kind" = none ] && kind=data ;;
            code/game/*|code/cgame/*|code/q3_ui/*|code/ui/*|game/*)
                [ "$kind" = engine ] || kind=module ;;
            *)
                kind=engine ;;
        esac
    done

    case "$kind" in
        none) return ;;
        data)
            # new top-level files under data/base need their build-tree symlink
            cmake --build "$(build_dir "$CONFIG")" --target skaar_data > /dev/null 2>&1
            if game_running; then
                # shellcheck disable=SC2086
                send_cmd "dev_reload$data_files" && status "$C_GRN" "data changed:$data_files -> dev_reload" \
                    || status "$C_YEL" "data changed:$data_files (couldn't reach the game)"
            else
                status "$C_DIM" "data changed:$data_files (game not running)"
            fi
            ;;
        map|module|engine)
            build || return
            if game_running && ! outputs_changed; then
                status "$C_DIM" "build ok ${BUILD_TIME}s, nothing changed"
                return
            fi
            mark_outputs
            if ! game_running; then
                start_game
                status "$C_GRN" "build ok ${BUILD_TIME}s, launched"
            elif [ "$kind" = engine ] || [ "$ALWAYS_RELAUNCH" = 1 ]; then
                stop_game
                start_game
                status "$C_GRN" "build ok ${BUILD_TIME}s, relaunched"
            elif [ "$kind" = module ]; then
                if send_cmd "map_restart 0" && send_cmd "vid_restart"; then
                    status "$C_GRN" "build ok ${BUILD_TIME}s, game modules hot-reloaded"
                    toast "build ok, game modules reloaded"
                else
                    status "$C_YEL" "build ok ${BUILD_TIME}s, hot-reload NOT sent (couldn't reach the game)"
                fi
            else
                # shellcheck disable=SC2086
                if send_cmd "dev_reload$map_files"; then
                    status "$C_GRN" "build ok ${BUILD_TIME}s, map reloaded:$map_files"
                else
                    status "$C_YEL" "build ok ${BUILD_TIME}s, map reload NOT sent (couldn't reach the game)"
                fi
            fi
            ;;
    esac
}

cleanup() {
    trap - INT TERM EXIT
    printf '\n'
    status "$C_DIM" "dev-watch stopping"
    exec 2>/dev/null    # no "Terminated" job notices for the helpers below
    [ -n "${WATCH_PID:-}" ] && kill "$WATCH_PID"
    pkill -f "tail -n 0 -F $LOGS/latest.log" 2>/dev/null
    pkill -f "fswatch -r -l 0.5 --batch-marker=__BATCH__" 2>/dev/null
    stop_game
    exit 0
}
trap cleanup INT TERM EXIT

WATCH_DIRS=
for d in code game tools cmake data CMakeLists.txt; do
    [ -e "$REPO/$d" ] && WATCH_DIRS="$WATCH_DIRS $REPO/$d"
done

printf '%sdev-watch (%s build): watching%s%s\n' "$C_DIM" "$CONFIG" "$(echo "$WATCH_DIRS" | sed "s|$REPO/||g")" "$C_OFF"

status "$C_DIM" "building..."
if build; then
    mark_outputs
    stop_game
    start_game
    status "$C_GRN" "build ok ${BUILD_TIME}s, launched"
fi

# Filtered live log (errors red, warnings yellow) for whatever session is newest
"$REPO/scripts/watch.sh" &
WATCH_PID=$!

# --batch-marker groups the paths of one debounced (500 ms) change burst
# shellcheck disable=SC2086
fswatch -r -l 0.5 --batch-marker=__BATCH__ \
    -e '/\.git/' -e '/build-' -e '/logs/' -e '/\.home/' -e '\.swp$' -e '~$' -e '\.DS_Store$' \
    $WATCH_DIRS | {
    batch=
    while IFS= read -r path; do
        if [ "$path" = __BATCH__ ]; then
            handle_batch "$batch"
            batch=
        else
            batch="$batch $path"
        fi
    done
} &

# The shell defers traps while a foreground pipeline runs; `wait` is
# interruptible, so Ctrl+C / SIGTERM reach cleanup() right away
wait $!
