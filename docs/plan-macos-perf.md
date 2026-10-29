# Plan: macOS window, load times, lean build, Skaar.app

Asked by the human 2026-10-29: lean and fast on macOS first, start in a window, quick fullscreen and Cmd+Tab,
a self-contained `Skaar.app` (notarization later). No feature may get worse.

## Baseline (M1, fast build, warm disk, `+set developer 1`)
| Step | ms |
|---|---|
| launch → SDL video up | 85 |
| window + GL context + show | 90 |
| joystick subsystem (unused) | 12 |
| GLSL: 115 programs | 310 |
| OpenAL device | 100 |
| OpenAL capture device (VoIP mic, unused) | 38 |
| **launch → main menu** | **~700** |
| map load: renderer restart recompiles all 115 programs | 290 of ~440 |

## Tasks
A. **Window**: start windowed at 854×480 points (Minecraft's default), centered, resizable. `r_fullscreen` is never
   restored at launch. Fullscreen on macOS is a borderless desktop-size window (no display mode switch, no Space
   animation), so Cmd+Tab is instant; toggled with Cmd+Ctrl+F, Cmd+Enter, Alt+Enter, the green button or Settings.
   Resizing and fullscreen never `vid_restart`: the renderer resizes its screen-sized render targets in place and the
   ui/cgame modules pick up the new size through the ROM cvar `r_vidSize`. Config migration v2 moves the old defaults.
B. **Load times**: GLSL programs survive map loads (recompiled only when a setting they depend on changes);
   no capture device unless VoIP is on; no joystick subsystem unless `in_joystick 1`.
C. **Lean**: remove code that is never built or never reached, and build options Skaar doesn't use.
D. **Skaar.app**: `scripts/package-app.sh` builds a release (LTO, dead-stripped) and assembles a self-contained bundle,
   game data inside `Contents/Resources`, ad-hoc signed.

## Done when
- launch → menu and map loads measurably faster than the baseline above; numbers recorded here.
- Window, fullscreen toggle and resize work without `vid_restart`; `./scripts/smoke.sh` passes.
- `Skaar.app` runs from anywhere with no repo next to it.

## Results (M1, fast build, foreground launch, warm disk)
| Step | before | after | how |
|---|---|---|---|
| SDL app delegate at launch | 300 ms sleep + Dock activation | 0 | `Sys_OSX_InitApp` replaces SDL's `applicationDidFinishLaunching:` |
| GLSL programs at launch | 310 ms | 0 (compiled on first use + idle menu frames) | deferred compile in `tr_glsl.c` |
| Network Huffman tables (first frame) | 50 ms | 0 (built on a thread at `Com_Init`) | `MSG_StartHuffman` |
| OpenAL device open | 40-100 ms | ~1 ms (opened on a thread during window setup) | `S_AL_WarmUp` / `S_AL_WarmUpTake` |
| Joystick subsystem, VoIP capture device | 12 + 38 ms | 0 | skipped unless enabled |
| **launch → first menu frame** | **~1000 ms** (700 + the 300 ms SDL sleep) | **~220-260 ms** | the rest is AppKit/SDL init (~95 ms) and window + GL context (~60 ms) |
| map load | ~440 ms | ~45-50 ms | GLSL programs kept across the renderer restart |

Same numbers from `Skaar.app` after its first launch (macOS scans a new binary once, ~200 ms).

Tried and dropped (measured, don't retry):
- A GLSL compile thread with a shared CGL context: Apple's GL holds the share-group lock while compiling,
  so the main thread's GL calls stall just the same (first frame 45 → 177 ms).
- Program binaries: `GL_NUM_PROGRAM_BINARY_FORMATS` is 0 on macOS.
So the menu spends its first ~1.3 s finishing GLSL programs one per frame (frames up to ~20-30 ms),
which keeps every map load at ~50 ms.

Lean: `-Wl,-dead_strip` on macOS links (release builds already had LTO and hidden visibility); unreferenced
functions in Skaar's own code removed (found with linker maps, checked against the source, since inlined
functions also show up as dead); unreferenced ioq3/Quake-branded files in `misc/` and committed `__pycache__` removed.
Not removed, pending a decision: the unbuilt upstream reference code (`code/game` except `bg_*`, `code/cgame`,
`code/q3_ui`, `code/ui`, top-level `ui/`), the QVM toolchain (`code/tools`), and the vendored Ogg/Vorbis/Opus
sources behind the now-OFF codec options.

## File claims
- **macos-perf**: `code/sdl/sdl_glimp.c`, `code/sdl/sdl_input.c` (window events, joystick init, menu cursor), `code/renderergl2/tr_glsl.c`,
  `tr_init.c`, `tr_image.c`, `tr_fbo.c`, `tr_cmds.c`, `tr_local.h`, `code/renderergl1/tr_init.c` (resize hook),
  `code/renderercommon/tr_public.h`, `tr_common.h`, `code/client/cl_main.c`, `cl_scrn.c`, `cl_keys.c`, `cl_input.c`, `cl_console.c`, `cl_cgame.c`, `cl_dev.c`, `client.h`, `code/ui/ui_public.h`, `code/null/null_client.c`,
  `snd_openal.c`, `snd_public.h`, `code/qcommon/cfg_migrate.c`, `common.c`, `msg.c`, `qcommon.h`, `q_shared.h`,
  `code/sys/sys_osx.m`, `sys_osx_window.m` (new), `sys_main.c`, `sys_local.h`, `win_resource.rc`,
  `data/base/default.cfg`, `game/shared/draw2d.*`, `charts.*`, `move_ruleset.*`, `game/ui/main.cpp`, `local.h`,
  `screen_settings.cpp`, `widgets.cpp`, `game/cgame/main.cpp`, `local.h`, `syscalls.*`, `game/sgame/syscalls.*`,
  `scripts/package-app.sh` (new), `scripts/skaar`, `cmake/` (build options, bundle identity), `CMakeLists.txt`,
  `misc/` (deletions), `.gitignore`, `docs/build-macos.md`, `docs/config-migrations.md`, `docs/dev-loop.md`, this file.
