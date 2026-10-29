#!/bin/sh
# Build a self-contained Skaar.app.  ./scripts/package-app.sh [output dir, default dist/]
#   Release build in build-release/: LTO, dead-stripped, no dev-only code,
#   universal (arm64 + x86_64) unless ARCH is set, e.g. ARCH=arm64.
#   Game data goes in Contents/Resources/base, the game modules in
#   Contents/MacOS/base, so the app runs from anywhere with nothing next to it.
#   Ad-hoc signed: runs on the Mac that built it. Other Macs need it signed
#   with a Developer ID and notarized.
set -eu
. "$(dirname -- "$0")/lib.sh"
[ "$OS" = Darwin ] || die "package-app.sh builds a macOS .app"

OUT=${1:-$REPO/dist}
DIR=$(build_dir release)
SRC=$DIR/Release
APP=$OUT/Skaar.app

GEN_ARGS=
command -v ninja >/dev/null 2>&1 && GEN_ARGS="-G Ninja"
LAUNCHER=
command -v ccache >/dev/null 2>&1 && LAUNCHER="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"

# shellcheck disable=SC2086
cmake -S "$REPO" -B "$DIR" $GEN_ARGS $LAUNCHER -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_OSX_ARCHITECTURES=${ARCH:-arm64;x86_64}" >/dev/null
cmake --build "$DIR" --target skaar renderer_opengl1 renderer_opengl2 \
    qagame_base cgame_base ui_base skaar_data skaar_maps

rm -rf "$APP"
mkdir -p "$OUT"
ditto "$SRC/skaar.app" "$APP"
mkdir -p "$APP/Contents/MacOS/base" "$APP/Contents/Resources/base"
for module in qagame cgame ui; do
    cp "$SRC/base/$module.dylib" "$APP/Contents/MacOS/base/"
done
# The build tree's base/ links into data/base: copy what the links point at
rsync -aL --exclude '*.dylib' --exclude '.DS_Store' "$SRC/base/" "$APP/Contents/Resources/base/"

# Inner code first, then the bundle
codesign --force --sign - "$APP/Contents/MacOS/base/"*.dylib "$APP/Contents/MacOS/"*.dylib
codesign --force --sign - "$APP"
codesign --verify --strict --deep "$APP"

printf '%s%s%s  %s, %s\n' "$C_GRN" "$APP" "$C_OFF" "$(du -sh "$APP" | cut -f1)" \
    "$(lipo -archs "$APP/Contents/MacOS/skaar")"
