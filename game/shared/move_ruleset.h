// Movement rulesets: the tunable half of player movement (docs/cpm-physics.md).
// Built in: "vq3" (stock Q3) and "cpm" (strafe-only air accel, air control,
// ramp and double jumps). data/base/rulesets/<id>.json overrides or adds to them.
//
// The active ruleset is an index in ps.stats[kStatRuleset], so server movement
// and client prediction always run the same one. The server owns the registry
// (LoadRulesets) and sends the active ruleset's values in kConfigstringRuleset;
// the client mirrors it at the same index (DecodeRuleset).
//
// This file is part of Skaar. GPLv2 or later.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace skaar::move {

// playerState_t.stats slots owned by movement. 0..6 are Q3's, 8..11 are the
// tac weapons' (P7, skaar-07); 7, 12 and 13 are free.
constexpr int kStatRuleset = 14;   // index into the registry below
constexpr int kStatJumpTime = 15;  // ms left in the double-jump window (networked as int16)

// Configstring with the server's active ruleset, as an info string
// (\index\2\id\cpm\accelerate\15...). 26 is unused in bg_public.h (between
// CS_BOTINFO and CS_ITEMS).
constexpr int kConfigstringRuleset = 26;

struct Ruleset {
    std::string id;     // file name without .json
    std::string label;  // shown in menus (no trademarked names)

    // ground
    float maxSpeed = 0;      // run speed; 0 = the server's g_speed (ps.speed)
    float accelerate = 10;
    float friction = 6;
    float stopSpeed = 100;   // friction acts as if at least this fast
    float duckScale = 0.25f;

    // air (Q3 behaviour when the cpm extras are 0)
    float airAccelerate = 1;
    float airStopAccelerate = 0;  // accel when wishing against the velocity; 0 = off
    float strafeAccelerate = 0;   // accel with only strafe keys held; 0 = off
    float strafeWishSpeed = 0;    // wishspeed cap with only strafe keys held; 0 = no cap
    float airControl = 0;         // turn toward the view with forward/back only; 0 = off
    float airControlPower = 2;    // exponent on the alignment term
    bool airControlBackward = true;
    float airSpeedCap = 0;        // air accel can't raise horizontal speed past this; 0 = no cap

    // jumping
    float jumpVelocity = 270;
    bool rampJump = false;        // keep upward velocity: vz = max(vz, 0) + jumpVelocity
    int doubleJumpWindowMs = 0;   // 0 = no double jump
    float doubleJumpBoost = 0;    // added to a jump inside the window
    bool autoHop = false;         // jump again without releasing the key

    // water, flight, spectator
    float swimScale = 0.5f;
    float waterAccelerate = 4;
    float waterFriction = 1;
    float flyAccelerate = 8;
    float flightFriction = 3;
    float spectatorFriction = 5;
};

const Ruleset& Vq3();
const Ruleset& Cpm();

// Parses data/base/rulesets/<id>.json. Keys left out keep the values of the
// built-in with the same id, or vq3's for a new id. Unknown keys are ignored.
bool ParseRuleset(std::string_view id, std::string_view json, Ruleset& out, std::string& error);

// Registry. Starts as {vq3, cpm}.
void ResetRulesets();
int AddRuleset(const Ruleset& ruleset);  // replaces one with the same id, else appends; returns its index
int FindRuleset(std::string_view id);    // -1 if unknown
const Ruleset& RulesetAt(int index);     // out of range: vq3
void SetRulesetAt(int index, const Ruleset& ruleset);  // grows the registry with vq3 copies if needed

// Registers every rulesets/<id>.json, in name order so the indices are stable.
// list returns file names in a directory; read returns a file's text. Bad
// files are skipped and reported in errors ("<file>: <message>").
void LoadRulesets(const std::function<std::vector<std::string>(const char* dir, const char* ext)>& list,
                  const std::function<bool(const std::string& path, std::string& text)>& read,
                  std::vector<std::string>& errors);

// Every value, as JSON that ParseRuleset reads back exactly.
std::string ToJson(const Ruleset& ruleset);

// kConfigstringRuleset contents. Decode registers the ruleset at its index.
// Labels lose the characters info strings can't carry (\ ; ").
std::string EncodeRuleset(int index);
bool DecodeRuleset(std::string_view configstring, std::string& error);

}  // namespace skaar::move
