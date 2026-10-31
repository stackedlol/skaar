# Dependencies

Every dependency, with the reason for it. Add an entry before adding a dependency.

## Package management
Vendored sources under `code/thirdparty/`, following the ioq3 convention. No vcpkg or CPM yet:
the engine's deps already ship in-tree, and vendoring keeps builds offline and reproducible.
Revisit if we add several C++ libraries (CPM.cmake would be the pick).

## Inherited from ioquake3 (in `code/thirdparty/`)
SDL2 2.32.8 (bundled dylib on macOS/Windows), zlib, libjpeg, libogg, libvorbis, opus/opusfile, OpenAL Soft headers, curl headers.
Licenses are compatible with GPLv2. See each directory.

## Added by Skaar
| Dependency | Version | License | Where | Why |
|---|---|---|---|---|
| nlohmann/json | 3.11.3 (`json.hpp`, sha256 `9bea4c80…103ea6`) | MIT | `code/thirdparty/nlohmann-json-3.11.3/` | JSON parsing for tools (mapc), later scenario files. The brief's stack choice. |

## Dev tools (not linked)
| Tool | Why |
|---|---|
| CMake ≥ 3.25, Ninja | build |
| ccache | fast rebuilds (`scripts/build.sh` uses it if present) |
| fswatch | file watching for `scripts/dev-watch.sh` |
| lldb (macOS) / gdb (Linux) | crash backtraces in `scripts/dev.sh` |
| perl | millisecond timing in dev-watch status lines (falls back to seconds without it) |
| Python 3 + Pillow **11.3.0** | only for regenerating placeholder assets (`tools/gen_assets.py`). Pin it: glyph metrics come from Pillow's bundled FreeType and move between builds - a fresh run with any Pillow we tried (8.4 through 11.3) writes advance 19.00 where the committed `jbmono-400.fontinfo` has 19.20, so no current Pillow reproduces the shipped atlases exactly - and `game/shared/draw2d.cpp` draws every string from those numbers. `gen_assets.py` therefore refuses to rewrite a `.fontinfo` whose metrics would change unless you pass `--allow-font-drift`, so a regeneration is a deliberate, reviewable diff. |
