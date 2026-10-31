#!/bin/sh
# Install Skaar from a fresh clone:
#   git clone https://github.com/stackedlol/skaar.git && cd skaar && ./install.sh
# then run `skaar` from any terminal.
#
# 1. checks for a C/C++ compiler (macOS: Xcode Command Line Tools) and CMake;
#    if CMake is missing or too old, installs it with Homebrew / the system
#    package manager, or falls back to a private copy in .tools/ (no sudo)
# 2. builds the game (build-fast/)
# 3. installs the `skaar` command into ~/.local/bin and puts that on your PATH
#
# Re-running is safe. `skaar update` pulls the latest code and rebuilds.
set -eu
REPO=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OS=$(uname -s)
ARCH=$(uname -m)
CMAKE_MIN=3.25
CMAKE_PORTABLE=3.31.6
TOOLS="$REPO/.tools"

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    B=$(printf '\033[1m'); R=$(printf '\033[31m'); Y=$(printf '\033[33m'); G=$(printf '\033[32m'); O=$(printf '\033[0m')
else
    B=; R=; Y=; G=; O=
fi
step() { printf '\n%s==> %s%s\n' "$B" "$*" "$O"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\n%serror: %s%s\n' "$R" "$*" "$O" >&2; exit 1; }

# version_ge 3.31.6 3.25 -> true
version_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -t. -k1,1n -k2,2n -k3,3n | head -1)" = "$2" ]; }

cmake_ok() {
    command -v cmake >/dev/null 2>&1 || return 1
    version_ge "$(cmake --version | head -1 | awk '{print $3}')" "$CMAKE_MIN"
}

# A private CMake in .tools/, verified against the release's SHA-256 list
install_portable_cmake() {
    case "$OS-$ARCH" in
        Darwin-*)        pkg=cmake-$CMAKE_PORTABLE-macos-universal; bin=CMake.app/Contents/bin ;;
        Linux-x86_64)    pkg=cmake-$CMAKE_PORTABLE-linux-x86_64; bin=bin ;;
        Linux-aarch64)   pkg=cmake-$CMAKE_PORTABLE-linux-aarch64; bin=bin ;;
        *) die "no prebuilt CMake for $OS $ARCH; install CMake $CMAKE_MIN+ yourself and re-run" ;;
    esac
    if [ ! -x "$TOOLS/$pkg/$bin/cmake" ]; then
        note "downloading CMake $CMAKE_PORTABLE into .tools/"
        url=https://github.com/Kitware/CMake/releases/download/v$CMAKE_PORTABLE
        mkdir -p "$TOOLS"
        tmp=$(mktemp -d "${TMPDIR:-/tmp}/skaar-cmake.XXXXXX")
        trap 'rm -rf "$tmp"' EXIT INT TERM   # a failed download must not leave 50 MB behind
        curl -fsSL -o "$tmp/$pkg.tar.gz" "$url/$pkg.tar.gz" || die "CMake download failed"
        curl -fsSL -o "$tmp/sums.txt" "$url/cmake-$CMAKE_PORTABLE-SHA-256.txt" || die "CMake checksum download failed"
        want=$(awk -v f="$pkg.tar.gz" '$2 == f {print $1}' "$tmp/sums.txt")
        if command -v shasum >/dev/null 2>&1; then got=$(shasum -a 256 "$tmp/$pkg.tar.gz" | awk '{print $1}')
        else got=$(sha256sum "$tmp/$pkg.tar.gz" | awk '{print $1}')
        fi
        [ -n "$want" ] && [ "$want" = "$got" ] || die "CMake checksum mismatch"
        tar -xzf "$tmp/$pkg.tar.gz" -C "$TOOLS"
        rm -rf "$tmp"
    fi
    mkdir -p "$TOOLS/bin"
    for t in cmake ctest; do ln -sf "$TOOLS/$pkg/$bin/$t" "$TOOLS/bin/$t"; done
    PATH="$TOOLS/bin:$PATH"
}

# sudo only when not already root
as_root() { if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi; }

printf '%sSkaar installer%s  (%s %s)\n' "$B" "$O" "$OS" "$ARCH"

# ---- 1. tools ------------------------------------------------------------------
step "Checking build tools"
[ -d "$TOOLS/bin" ] && PATH="$TOOLS/bin:$PATH"
case "$OS" in
    Darwin)
        if ! xcode-select -p >/dev/null 2>&1; then
            note "the Xcode Command Line Tools (Apple's compiler) are not installed;"
            note "opening Apple's installer. When it finishes, run ./install.sh again."
            xcode-select --install 2>/dev/null || true
            exit 1
        fi
        note "compiler: $(cc --version | head -1)"
        if ! cmake_ok && command -v brew >/dev/null 2>&1; then
            note "installing cmake and ninja with Homebrew"
            brew install cmake ninja
        elif cmake_ok && ! command -v ninja >/dev/null 2>&1 && command -v brew >/dev/null 2>&1; then
            brew install ninja || true    # optional, just faster
        fi
        ;;
    Linux)
        missing=
        command -v c++ >/dev/null 2>&1 || missing="$missing compiler"
        cmake_ok || missing="$missing cmake"
        [ -f /usr/include/SDL2/SDL.h ] || missing="$missing sdl2"
        if [ -n "$missing" ]; then
            note "missing:$missing; installing packages (may ask for your password)"
            if command -v apt-get >/dev/null 2>&1; then
                as_root apt-get update
                as_root apt-get install -y build-essential cmake ninja-build libsdl2-dev curl
            elif command -v dnf >/dev/null 2>&1; then
                as_root dnf install -y gcc gcc-c++ make cmake ninja-build SDL2-devel curl
            elif command -v pacman >/dev/null 2>&1; then
                as_root pacman -S --needed --noconfirm base-devel cmake ninja sdl2 curl
            else
                die "unknown package manager; install a C++ compiler, CMake $CMAKE_MIN+ and SDL2 headers, then re-run"
            fi
        fi
        ;;
    *)
        die "$OS isn't supported by this script yet (macOS and Linux only). Windows: see docs/ for a CMake + Visual Studio build."
        ;;
esac
# still no (new enough) CMake, e.g. no Homebrew, or an old distro package
cmake_ok || install_portable_cmake
cmake_ok || die "CMake $CMAKE_MIN+ not available"
note "cmake: $(cmake --version | head -1 | awk '{print $3}')$(command -v ninja >/dev/null 2>&1 && echo ' + ninja')"

# ---- 2. build ------------------------------------------------------------------
step "Building Skaar (the first build takes a few minutes)"
"$REPO/scripts/skaar" build fast || die "build failed; the log is in logs/skaar-build.log"

# ---- 3. the `skaar` command -----------------------------------------------------
step "Installing the skaar command"
BIN_DIR="$HOME/.local/bin"
"$REPO/scripts/install-command.sh" "$BIN_DIR" >/dev/null
note "installed $BIN_DIR/skaar"

case ":$PATH:" in
    *":$BIN_DIR:"*) on_path=1 ;;
    *)
        on_path=0
        unmanaged=
        line="export PATH=\"$BIN_DIR:\$PATH\""
        case "$(basename "${SHELL:-sh}")" in
            fish) mkdir -p "$HOME/.config/fish"
                  rc="$HOME/.config/fish/config.fish"
                  line="fish_add_path \"$BIN_DIR\"" ;;
            zsh)  rc="$HOME/.zshrc" ;;
            bash) if [ "$OS" = Darwin ]; then rc="$HOME/.bash_profile"; else rc="$HOME/.bashrc"; fi ;;
            *)    rc="$HOME/.profile"; unmanaged=1 ;;
        esac
        if ! grep -qs 'added by the Skaar installer' "$rc"; then
            printf '\n# added by the Skaar installer\n%s\n' "$line" >> "$rc"
            if [ -n "$unmanaged" ]; then
                note "added $BIN_DIR to $rc, but this installer doesn't know $(basename "${SHELL:-sh}"): add it to your PATH yourself"
            else
                note "added ~/.local/bin to your PATH in $rc"
            fi
        fi
        ;;
esac

printf '\n%s%sDone.%s\n' "$G" "$B" "$O"
if [ "$on_path" = 1 ]; then
    note "Play:    skaar"
elif [ -n "${unmanaged:-}" ]; then
    note "Play now: $BIN_DIR/skaar      (until $BIN_DIR is on your PATH)"
else
    note "Open a new terminal window, then play with:  skaar"
    note "(or right now: $BIN_DIR/skaar)"
fi
note "Update:  skaar update      Help: skaar help"
note "Errors from the game print in that terminal; crash reports go to $REPO/logs/"
