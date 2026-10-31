#!/bin/sh
# Build Skaar.  ./scripts/build.sh [fast|safe] [extra cmake --build args]
#   fast: -O2 -g, for judging feel and framerate        -> build-fast/
#   safe: -O0 -g + AddressSanitizer + UBSan, for bugs   -> build-safe/
# Both are dev builds (SKAAR_DEV: DEV_ASSERT, dev_crash), arm64-only on macOS,
# LTO off, ccache on. Warnings are errors in our own code (cmake/skaar/dev.cmake).
set -eu
. "$(dirname -- "$0")/lib.sh"

CONFIG=${1:-fast}
[ $# -gt 0 ] && shift
TYPE=$(build_type "$CONFIG")
DIR=$(build_dir "$CONFIG")

# Can this toolchain run an ASan binary? An old ASan runtime can deadlock at
# startup on a newer macOS (docs/macos-notes.md); fall back to UBSan only.
asan_works() {
    probe_dir=$(mktemp -d "${TMPDIR:-/tmp}/skaar-asan.XXXXXX")
    trap 'rm -rf "$probe_dir"' EXIT INT TERM   # an interrupt must not leave it behind
    printf 'int main(void){return 0;}\n' > "$probe_dir/p.c"
    ok=1
    if cc -fsanitize=address "$probe_dir/p.c" -o "$probe_dir/p" 2>/dev/null; then
        "$probe_dir/p" & pid=$!
        n=0
        while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
        if kill -0 "$pid" 2>/dev/null; then kill -9 "$pid" 2>/dev/null; ok=0
        else wait "$pid" || ok=0
        fi
    else
        ok=0
    fi
    rm -rf "$probe_dir"
    [ "$ok" = 1 ]
}

case "$CONFIG" in
    fast) FLAGS="-O2 -g -DNDEBUG"; SANITIZE= ;;
    safe)
        FLAGS="-O0 -g -fno-omit-frame-pointer"
        if asan_works; then
            SANITIZE=address,undefined
        else
            SANITIZE=undefined
            printf '%sWARNING: AddressSanitizer hangs with this toolchain; safe build uses UBSan only. Update the Command Line Tools (docs/macos-notes.md).%s\n' "$C_YEL" "$C_OFF" >&2
        fi
        ;;
esac

GEN_ARGS=
command -v ninja >/dev/null 2>&1 && GEN_ARGS="-G Ninja"
LAUNCHER=
command -v ccache >/dev/null 2>&1 && LAUNCHER="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
ARCH=
[ "$OS" = Darwin ] && ARCH="-DCMAKE_OSX_ARCHITECTURES=$(uname -m)"

# (re)configure on first use, when this script (i.e. the flags) changed, or
# when the sanitizer choice changed (e.g. after a toolchain update)
if [ ! -f "$DIR/CMakeCache.txt" ] || [ "$0" -nt "$DIR/CMakeCache.txt" ] ||
   ! grep -q "^SKAAR_SANITIZE:STRING=$SANITIZE\$" "$DIR/CMakeCache.txt"; then
    # shellcheck disable=SC2086
    cmake -S "$REPO" -B "$DIR" $GEN_ARGS $LAUNCHER $ARCH \
        -DCMAKE_BUILD_TYPE="$TYPE" \
        "-DCMAKE_C_FLAGS_$(echo "$TYPE" | tr '[:lower:]' '[:upper:]')=$FLAGS" \
        "-DCMAKE_CXX_FLAGS_$(echo "$TYPE" | tr '[:lower:]' '[:upper:]')=$FLAGS" \
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
        -DSKAAR_DEV=ON "-DSKAAR_SANITIZE=$SANITIZE" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
fi

# Ninja is parallel by default; make is not
if [ -z "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ] && ! grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja$' "$DIR/CMakeCache.txt"; then
    CMAKE_BUILD_PARALLEL_LEVEL=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
    export CMAKE_BUILD_PARALLEL_LEVEL
fi

cmake --build "$DIR" "$@"
