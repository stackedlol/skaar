# Scenarios and benchmarks

Aim-trainer scenarios are JSON files. Anyone can add or tune one without touching code.

The quickest way is in game: Play › pick a scenario › Customize. The editor tunes duration, fire mode,
targets, size, health, lifetime, distance, spread, movement, speed and your own movement and physics. It saves a
copy as `scenarios/<id>-mine.json` in your home dir. For anything it doesn't cover, edit that file by hand.

- Built-in: `data/base/scenarios/<id>.json` (`<id>` is the file name and shows up in stats).
- Your own: put the file in your home dir, `~/Library/Application Support/Skaar/base/scenarios/` on macOS
  (`~/.local/share/Skaar/base/scenarios/` on Linux; `~/.skaar/base/` if that folder already exists). The game reads both. A home file with a built-in's name overrides it.
- **Packs:** zip a `scenarios/` folder (and `benchmarks/`, if the pack has one) and rename the zip to `<name>.pk3`.
  Drop it into `base/` in your home dir. The game mounts every pk3 at start like a folder, and the pack's
  scenarios show up in Play. Files that don't parse are listed under the scenario list instead of loading.
- Comments (`//`) are allowed. Unknown keys are ignored. Bad values show an error in the scenario list instead of loading.

## Scenario
```jsonc
{
  "name": "Wall grid",                  // shown in menus
  "category": "click",                  // click | track | switch (free text: groups the list and the stats)
  "description": "One or two sentences.",
  "duration": 60,                       // seconds, 5..600
  "map": "range",                       // map to play on (default "range")
  "seed": 0,                            // 0 = new layout every run; any other number = same layout every run
  "player": {
    "move": false,                      // false: frozen in place, view still turns
    "ruleset": "vq3"                    // movement physics when you can move: vq3 | cpm | any rulesets/<id>.json
  },
  "fire": "click",                      // click: one shot per press | hold: continuous beam
  "weapon": "rifle",                    // optional: a real gun (data/base/weapons): its refire, spray and spread,
                                        // scored per shot like clicks
  "targets": {
    "alive": 3,                         // on screen at once, 1..16
    "radius": 22,                       // world units (the player is 30 wide)
    "health": 1,                        // click: hits to kill | hold: seconds on target to kill
    "lifetime": 0,                      // seconds before a target vanishes unhit (0 = never, else >= 0.05)
    "spawn": {
      "shape": "grid",                  // cone: random in the window | grid: cols × rows cells over the window
      "cols": 5, "rows": 5,
      "distance": 700,                  // number or [min, max], world units from the eye (>= 64)
      "yaw": [-16, 16],                 // degrees around the spawn facing, left is +
      "pitch": [-6, 14]                 // degrees, up is +
    },
    "motion": {
      "type": "strafe",                 // static | strafe (left/right) | float (any direction)
      "speed": [260, 420],              // world units per second, re-rolled at each change (|speed| <= 5000)
      "changeEvery": [0.3, 1.1]         // seconds between direction changes
    }
  },
  "score": {
    "kill": 100,                        // points per kill
    "accuracyWeight": 0.5,              // click: score = kills × kill × ((1 − w) + w × accuracy), w 0..1
    "perSecondOnTarget": 100            // hold: score = seconds on target × this + kills × kill
  }
}
```
Moving targets stay inside the spawn window and bounce off its edges.

### Stats each run records
Score, accuracy (click: hits / shots; hold: time on target / time firing), kills, mean time-to-kill (spawn → kill),
every shot's aim error relative to the nearest target, and the view direction at 60 Hz. See `game/shared/runlog.h`.

## Benchmark
A benchmark groups scenarios and sets a score for each tier. The insights screen shows your tier per scenario and
picks the one to focus on next (your lowest standing).
```jsonc
{
  "name": "Core",
  "description": "Seven scenarios across clicking, tracking and switching.",
  "tiers": ["Spark", "Ember", "Flare", "Blaze", "Nova", "Zenith"],   // lowest first
  "scenarios": [
    { "id": "wall-grid", "thresholds": [5000, 7000, 9000, 11000, 13000, 15000] }   // one per tier, ascending
  ]
}
```
Files: `data/base/benchmarks/<id>.json`, or `benchmarks/` in your home dir.
The `core` thresholds are first guesses. They get calibrated once real runs exist.
