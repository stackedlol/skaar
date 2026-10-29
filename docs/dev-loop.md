# Dev loop

How to build, run and test Skaar with every error visible. Implements `docs/DEV_ENV.md` Part B.

## TL;DR
```sh
./scripts/dev-watch.sh        # build, launch into the last map, rebuild/reload on every save
./scripts/watch.sh            # optional second terminal: only errors/warnings, colored
./scripts/smoke.sh            # before every commit: must print SMOKE PASS
```
If the game crashes, paste the newest `logs/crash-*.txt` to the agent.

## Scripts
| Script | What it does |
|---|---|
| `scripts/build.sh [fast\|safe]` | `fast`: `-O2 -g`, for feel/framerate → `build-fast/`. `safe`: `-O0 -g` + ASan + UBSan → `build-safe/`. Both are dev builds (`SKAAR_DEV`), arm64-only on macOS, LTO off, ccache on. Re-configures when the script changes. |
| `scripts/dev.sh [fast\|safe] [args]` | Launches the client with dev cvars, logs to `logs/session-*.log` (`logs/latest.log` → newest), under lldb/gdb. Without `+map`/`+devmap` args, it loads the map from `logs/last-session.cfg` (default `devmap testroom`). |
| `scripts/watch.sh [--all]` | `tail -F logs/latest.log`, filtered: red = ERROR/FATAL/ASSERT/sanitizer/crash, yellow = WARNING/spike. Bursts collapse to "+N more like this". |
| `scripts/smoke.sh` | Builds `safe`, runs a headless dedicated server on `testroom` for 20 s, quits it, and fails on early exit, bad exit status, map not loaded, or any ERROR/ASSERT/sanitizer line. |
| `scripts/rebuild.sh [fast\|safe]` | Rebuilds only the game modules and hot-reloads them in the running game. |
| `scripts/dev-watch.sh [fast\|safe] [args]` | The auto rebuild loop (below). |

## dev-watch: what a save triggers
Watches `code/ game/ tools/ cmake/ data/ CMakeLists.txt` with fswatch, 500 ms debounce.

| Changed | Action |
|---|---|
| `code/game`, `code/cgame`, `code/q3_ui`, `code/ui`, `game/` | build → hot-reload modules (`map_restart 0` + `vid_restart`), no relaunch |
| `data/maps-src/*.json` | build (mapc) → `dev_reload maps/<name>.bsp` → map reloads if it's the current one |
| `data/base/*` | no build → `dev_reload <file>`: `.cfg` is exec'd, images/shaders → `vid_restart`, sounds → `snd_restart` |
| anything else (engine C, CMake, tools) | build → close the game → relaunch into the same map |

- A failed build never relaunches. The last working game keeps running. Compiler errors print in red, and the first one is toasted in-game.
- After each cycle it prints one status line, e.g. `[16:24:37] build ok 1.2s, game modules hot-reloaded` or `[16:24:52] build FAILED in 0.9s, 2 error(s)`.
- Status lines are also appended to `logs/latest.log`.
- `DEV_WATCH_RELAUNCH=1` relaunches on every successful build instead of hot-reloading.
- Ctrl+C stops the watcher and closes the game.

Commands reach the running game through `com_pipefile` (`.home/base/dev.pipe`, a named pipe the engine polls every frame).
Send your own: `printf 'dev_overlay 1\n' > .home/base/dev.pipe`.

### Module reload (verified 2026-10-22 on macOS)
- `map_restart 0` → `SV_RestartGameProgs` → `VM_Restart`: native qagame is dlclosed and dlopened again, so the new code runs.
  (`map_restart` without the `0` waits for a 5 s warmup countdown first.)
- `vid_restart` shuts down and re-creates cgame and ui, which dlopens the rebuilt dylibs.
- `map <name>` / `devmap <name>` reloads qagame and cgame, but not ui.
- Relinking a dylib while the game has it loaded is safe (the linker replaces the file, it doesn't rewrite it in place).

### Known limits
- Engine changes (`code/client`, `code/qcommon`, `code/server`, `code/sys`, renderers) need a full relaunch. dev-watch does that automatically.
- Hot reload resets module state (it's a map restart). Relaunch if something looks stale.
- New top-level entries in `data/base/` need the `skaar_data` link step. dev-watch runs it before `dev_reload`.
- `com_pipefile` doesn't work on Windows (upstream limitation).

## In-game diagnostics (engine side, works without any game module)
| Feature | How to see it |
|---|---|
| Toasts | Off by default (warnings stream to the terminal). `dev_toasts 1`: any WARNING / ERROR / ASSERT line shows small in the bottom-right for 5 s (newest 3, newest at the bottom). |
| Error panel | After an `ERR_DROP` with no ui module loaded: full-screen red panel with the message and the session log path, for 10 s or until the next map loads. With the ui loaded, the menu shows the error itself. |
| Fatal errors | Native dialog with the message, crash report path and session log path. `dev_errorDialog 0` suppresses it (headless runs). |
| `dev_overlay 1` | Corner panel: fps, frame time, server tick rate, ping, entity count, last 3 warnings. |
| Frame spikes | `spike: frame X ms (avg Y ms): events, server (ticks), client+render` when a frame's work exceeds 2× the running average and `dev_spikeMinMs` (default 12; on macOS with uncapped fps, 8.3 ms frames are just the 120 Hz swap). |
| Tick backlog | `WARNING: server tick loop behind` when the server's fixed-step accumulator exceeds `dev_maxTickBacklog` ticks (default 3). |
| `sens_test` | Run it, turn exactly 360° (or move the mouse a measured distance), then run it again. Prints counts, degrees, measured vs expected counts/360, and cm/360 if `sens_dpi` is set. Compare the reported mouse travel with a ruler to detect OS acceleration. |
| `dev_notes` | Lists recent warnings/errors in the console. |
| `dev_crash <segv\|abort\|fpe\|assert\|warning\|drop\|fatal\|spike>` | Triggers each failure path on purpose (dev builds only). |

## DEV_ASSERT
```c
#include "../qcommon/dev_assert.h"
DEV_ASSERT( ent->health >= 0, "negative health" );
```
In dev builds a failed check logs `ASSERT code/game/g_combat.c:123 negative health (ent->health >= 0)` plus a backtrace, shows a toast, and keeps running.
Repeats from the same call site log on the 1st, 10th, 100th... failure. In release builds it compiles to nothing.
Works in the engine and in game modules (C and C++).

## Crash reports
Signals (SEGV, BUS, FPE, ILL, ABRT) and fatal errors write `logs/crash-<timestamp>.txt` with:
- the signal or error, version, map, mode (gametype, ruleset)
- OS, CPU and GPU strings, and the session log path
- a backtrace (`backtrace_symbols_fd`)
- the last 200 console lines

Then the normal shutdown runs. Under lldb/gdb (`dev.sh`), the debugger also prints `bt all` into the session log first.
To resolve a frame to file:line without a debugger: `atos -o build-fast/RelWithDebInfo/skaar.app/Contents/MacOS/skaar -l <load address> <address>`.

## Dev cvars passed by dev.sh
All verified to exist in this ioq3 version:
`developer 1`, `logfile 2` (qconsole.log flushed per write), `com_hunkMegs 256`, `com_pipefile dev.pipe`,
`fs_basepath <build>/<config>`, `fs_homepath .home`, plus the Skaar ones: `com_abnormalExitDialog 0`, `dev_logdir logs`, `dev_logpath <session log>`.
`.home/` holds the dev configs, screenshots and the pipe. Delete it to reset to defaults.

## Ubuntu
The same scripts work. `dev.sh` uses gdb when lldb isn't available (`gdb -batch -ex run -ex "thread apply all bt full" -ex continue`).
Install `fswatch` in addition to the DEV_ENV list: `sudo apt install fswatch`.

## Soak test
`SOAK_MINUTES=120 ./scripts/soak.sh [safe|fast]` plays a long scripted session in its own home (`.home-soak`, background window,
no mouse, no network): every trainer scenario, the movement map on both rulesets with the jump pad, the test room and the
arena (firing, weapon switching). Every few seconds it samples resident memory, fps and frame p99 into `logs/soak-*.csv`.
It fails on a crash or early exit, any ERROR / ASSERT / sanitizer line, or memory that keeps growing
(> 25% and > 64 MB from the first to the last tenth). Run it before releases; it isn't part of `smoke.sh`.
