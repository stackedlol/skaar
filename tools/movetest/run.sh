#!/bin/sh
# Build and run the headless movement tests (tools/movetest/movetest.cpp).
#   tools/movetest/run.sh            exit status 0 = all passed
# Builds into build-movetest/ with the game modules' compilers. FP contraction is
# off so the parity test compares the C and C++ code paths, not FMA choices.
set -eu
REPO=$(cd "$(dirname -- "$0")/../.." && pwd)
OUT="$REPO/build-movetest"
mkdir -p "$OUT"
# work from the output dir so the object list below stays free of the space a
# checkout path may contain (the flags and sources are quoted one word each)
cd "$OUT"

CC=${CC:-cc}
CXX=${CXX:-c++}
set -- -O2 -g -ffp-contract=off -DQAGAME "-I$REPO/code" "-I$REPO/game" "-I$REPO/game/shared"

objs=
for src in qcommon/q_math.c qcommon/q_shared.c qcommon/dev_assert.c game/bg_misc.c game/bg_pmove.c game/bg_slidemove.c; do
    obj="$(basename "$src" .c).o"
    "$CC" "$@" -w -c "$REPO/code/$src" -o "$obj"
    objs="$objs $obj"
done

"$CXX" -std=c++20 "$@" "-I$REPO/code/thirdparty/nlohmann-json-3.11.3" -Wall -Wextra -Werror \
    "$REPO/tools/movetest/movetest.cpp" \
    "$REPO/game/shared/move_pmove.cpp" "$REPO/game/shared/move_slide.cpp" "$REPO/game/shared/move_ruleset.cpp" \
    $objs -o movetest  # shellcheck disable=SC2086 # bare object names in the cwd

exec ./movetest "$REPO"
