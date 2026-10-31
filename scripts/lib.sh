# Shared helpers for scripts/*.sh. POSIX sh; sourced, not executed.
# shellcheck shell=sh

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LOGS="$REPO/logs"
# fs_homepath for dev runs: configs, screenshots, pipe. Automated/agent test
# runs set SKAAR_DEV_HOME to their own dir so their +set overrides (windowed,
# small window) never get archived into the human's config.
HOMEDIR="${SKAAR_DEV_HOME:-$REPO/.home}"

OS=$(uname -s)

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_RED=$(printf '\033[31m'); C_YEL=$(printf '\033[33m'); C_GRN=$(printf '\033[32m')
    C_DIM=$(printf '\033[2m'); C_OFF=$(printf '\033[0m')
else
    C_RED=; C_YEL=; C_GRN=; C_DIM=; C_OFF=
fi

die() { printf '%s%s%s\n' "$C_RED" "$*" "$C_OFF" >&2; exit 1; }
now() { date +%H:%M:%S; }

# build_type <fast|safe> -> CMake build type (also the output subdir name)
build_type() {
    case "$1" in
        fast) echo RelWithDebInfo ;;
        safe) echo Debug ;;
        *) die "unknown build config '$1' (use fast or safe)" ;;
    esac
}

build_dir() { echo "$REPO/build-$1"; }
out_dir() { echo "$(build_dir "$1")/$(build_type "$1")"; }

client_bin() {
    if [ "$OS" = Darwin ]; then
        echo "$(out_dir "$1")/skaar.app/Contents/MacOS/skaar"
    else
        echo "$(out_dir "$1")/skaar"
    fi
}

server_bin() { echo "$(out_dir "$1")/skaarded"; }

# Engine args every dev run uses
dev_engine_args() {
    echo "+set fs_basepath $(out_dir "$1") +set fs_homepath $HOMEDIR +set com_abnormalExitDialog 0 +set dev_logdir $LOGS"
}

# The named pipe the engine reads commands from (com_pipefile, created in the game dir)
PIPE_NAME=dev.pipe
pipe_path() { echo "$HOMEDIR/base/$PIPE_NAME"; }

# pipe_write <fifo> <line...>: never blocks and returns 1 if the reader is gone.
# Opening a FIFO for writing with no reader blocks forever, and a crashed game
# leaves its pipe on disk, so write from a background process and give up on it.
pipe_write() {
    p=$1
    shift
    [ -p "$p" ] || return 1
    ( printf '%s\n' "$*" > "$p" ) 2>/dev/null &
    w=$!
    sleep 0.2
    if kill "$w" 2>/dev/null; then
        kill -9 "$w" 2>/dev/null
        return 1
    fi
    return 0
}

# Send a console command to the running game, if any. Never blocks.
send_cmd() { pipe_write "$(pipe_path)" "$*"; }
