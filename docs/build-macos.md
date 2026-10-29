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
