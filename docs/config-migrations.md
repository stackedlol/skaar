# Config migrations (sk_cfgVersion)

Players' configs (`skaarconfig.cfg`) store every archived cvar and every bind. When we change a default on purpose,
an existing config would keep the old value forever. Two Phase 1 bugs came from exactly that: the crosshair colour and the tonemap.

`code/qcommon/cfg_migrate.c` fixes it. Right after the config is executed (before `autoexec.cfg` and command-line
`+set`, which still win), it upgrades the config from its `sk_cfgVersion` to the current version:
- A **cvar row** changes a cvar only if it still has one of the listed old defaults. A value the player picked stays.
- A **bind row** rebinds a key only if it's bound to the old command, or with `""`, only if it's unbound.
- `Cfg_MigrateSpecial` handles rules that look at several cvars (e.g. the old default window → fullscreen).
- A fresh install (no config) starts at the current version and migrates nothing.
- Every change is printed: `config v1: cg_crosshairColor "232323" -> "f7e08a"`.

## Adding a migration
1. Change the default where it lives (module cvar registration, `game/shared/*` defaults, `data/base/default.cfg`).
2. Bump `CFG_VERSION` in `cfg_migrate.c` (once per batch, e.g. per release).
3. Add rows with that version:
   ```c
   { 2, "cg_fov", "90", "103" },                  // cvar: old default(s), space separated → new default
   { 2, "e", "", "+use" },                        // bind: "" = only if unbound
   ```
4. Test: put an old config in a scratch home and run
   `SKAAR_DEV_HOME=$PWD/.home-migtest ./scripts/dev.sh fast --menu +set r_fullscreen 0 +set r_mode -1 +quit`,
   then check the rewritten `.home-migtest/base/skaarconfig.cfg`.

Rows are cumulative: a v0 config runs every row, a v1 config only the v2+ rows.

## Version history
| Version | Date | Changes |
|---|---|---|
| 1 | 2026-10-22 | Reticle (colour f7e08a, size 4, gap 0); r_toneMap 0, r_autoExposure 0; neutral team names; vm_* native; old default window → fullscreen desktop; binds: r sk_restart, i sk_insights, 3/5/6/7 weapons, wheel weapprev/weapnext |
| 2 | 2026-10-29 | Window: r_mode -2 → -1 at 854x480 (from 1600x900 / 1600x1024), r_allowResize 1, r_centerWindow 1. Not a migration but related: `r_fullscreen` is reset to 0 at every launch (`Com_Init`), fullscreen is a toggle |
| 3 | 2026-10-29 | `dev_toasts` 1 → 0: toasts are opt-in in every build |
