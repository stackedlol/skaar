# Building Skaar on macOS (Apple Silicon)

Tested: MacBook Pro M2 Pro, macOS 27, Apple clang 17, CMake 4.0.3, Ninja.

## One-time setup
```sh
xcode-select --install
brew install cmake ninja sdl2 git ccache fswatch
```
SDL2 from Homebrew is optional: the client links ioq3's bundled universal SDL2 dylib
(`code/thirdparty/libs/macos`) by default (`USE_INTERNAL_SDL=ON`) and copies it into the app bundle.

## Build
Day to day, use the scripts (see `docs/dev-loop.md`). They wrap these commands:
```sh
cmake -S . -B build-fast -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache
cmake --build build-fast
```
Leaving out `CMAKE_OSX_ARCHITECTURES` builds universal arm64+x86_64 binaries (for release builds). That takes twice as long.

## Output (`build-fast/RelWithDebInfo/`)
| Path | What |
|---|---|
| `skaar.app/Contents/MacOS/skaar` | client (renderers + SDL2 dylib alongside) |
| `skaarded` | dedicated server |
| `base/{cgame,qagame,ui}.dylib` | native game modules |
| `base/*` symlinks | point at `data/base/*` (see `cmake/skaar/data.cmake`) |

## Skaar.app (self-contained)
```sh
./scripts/package-app.sh [dir]     # or: ./scripts/skaar app [dir]; default dir: dist/
```
A Release build in `build-release/` (LTO, `-dead_strip`, no `SKAAR_DEV` code), universal unless `ARCH=arm64`.
The game data is copied into `Contents/Resources/base` and the game modules into `Contents/MacOS/base`, so the
app runs from anywhere: the engine uses `Contents/Resources` as `fs_basepath` when it holds a `base/`
(`Sys_StripAppBundle`), and finds the modules through `fs_apppath`. Configs go to
`~/Library/Application Support/Skaar`. The app is ad-hoc signed, which runs on the Mac that built it;
other Macs need a Developer ID signature and notarization (`docs/macos-codesign-notarization.txt`).

## Window
Every launch starts windowed at 854×480 points, centered, resizable (`r_mode -1`). Cmd+Ctrl+F, Cmd+Enter,
Alt+Enter, the green title bar button and Settings toggle fullscreen, which on macOS is a borderless window
over the display with the menu bar and Dock hidden (no display mode change, no fullscreen Space), so Cmd+Tab
is instant and brings the other app's menu bar and Dock back. Resizing and toggling
never `vid_restart`: the renderer resizes its render targets in place.
In a window, menus use the normal macOS cursor and leave the mouse free; the mouse is only captured while
playing (Escape or Cmd+Tab frees it). Fullscreen menus keep the captured mouse and the game's own pointer.

## Run by hand
```sh
B=build-fast/RelWithDebInfo
$B/skaar.app/Contents/MacOS/skaar +set fs_basepath $PWD/$B +set fs_homepath $PWD/.home \
  +set com_abnormalExitDialog 0 +set developer 1
```
- `fs_homepath` keeps configs, logs and screenshots in the repo (`.home/`, gitignored) instead of `~/Library/Application Support/Skaar`.
- `com_abnormalExitDialog 0` is a Skaar cvar that skips the modal "didn't exit properly" dialog after a killed run.

## Build options changed from upstream ioq3
| Option | ioq3 | Skaar | Why |
|---|---|---|---|
| `BUILD_STANDALONE` | OFF | ON | Our own identity, no pak0 / CD key checks |
| `BUILD_GAME_QVMS` | ON | OFF | Modules are native (C++ planned); QVM can't compile C++ |
| `BUILD_MISSIONPACK` | (always) | OFF | Team Arena isn't used |
| `vm_game/vm_cgame/vm_ui` default | 2 (QVM) | 0 (native) | Same as above |
| `CMAKE_OSX_ARCHITECTURES` | forced universal | overridable | Faster arm64 dev builds |
| `USE_CODEC_VORBIS`, `USE_CODEC_OPUS` | ON | OFF | No Ogg/Opus assets; `.wav` only |
| `USE_VOIP`, `USE_MUMBLE` | ON | OFF | Opened the microphone at every launch; no voice chat yet |
