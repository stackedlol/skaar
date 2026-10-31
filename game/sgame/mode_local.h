// Internals of the arena mode (P6) shared by mode.cpp, weapons_fire.cpp,
// items.cpp and dummies.cpp. The damage rules follow Q3's g_combat.c (GPL).
//
// This file is part of Skaar. GPLv2 or later.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "local.h"
#include "mode.h"
#include "shared/arena.h"
#include "shared/weapons.h"

namespace sg::mode {

// Per-entity state the base gentity_t doesn't carry, indexed by entity number
struct EntState {
    enum class Kind { kNone, kItem, kMissile, kDummy, kTemp };
    Kind kind = Kind::kNone;
    int freeAt = 0;      // temp events: free after EVENT_VALID_MSEC
    int eventTime = 0;   // s.event set at this time; cleared after EVENT_VALID_MSEC

    // items
    int item = -1;
    int respawnAt = 0;   // hidden until then (0: available)

    // missiles
    int owner = ENTITYNUM_NONE;
    int slot = 0;
    int dieAt = 0;

    // dummies
    skaar::arena::DummyPattern pattern = skaar::arena::DummyPattern::kStand;
    vec3_t home{};
    float homeYaw = 0;
    vec3_t velocity{};
    int health = 0, armor = 0;
    int deadUntil = 0;   // respawn time while dead
    int nextTurn = 0;
    int nextJump = 0;
    float side = 1;      // strafe direction
    float heading = 0;   // circle pattern, degrees
    bool onGround = false;
};

// Per-client stats for the session (shown on the HUD, saved later)
struct ClientState {
    int lastDecay = 0;   // health / armor above 100 tick down once a second
    std::array<int, 16> shots{};
    std::array<int, 16> hits{};
    int damageDealt = 0;
    int damageTaken = 0;
    int kills = 0;
    int deaths = 0;
    std::array<int, 16> reserve{};  // tac weapons: rounds outside the magazine, per slot
    int lastShotMs = 0;      // command time of the last shot, unmasked (kStatLastShot is 16-bit)
    bool inSession = false;  // spawned on an arena map; written to stats/arena.jsonl at shutdown
    int sessionStart = 0;
};

struct Cvars {
    vmCvar_t knockback;     // g_knockback (Q3: 1000)
    vmCvar_t infiniteAmmo;  // g_infiniteAmmo
    vmCvar_t loadout;       // g_loadout: 1 = spawn with every weapon (practice), 0 = pick them up
};

// The map's loadout (info_loadout): weapon ids given on spawn, in order; empty = the arena default
struct MapRules {
    std::vector<std::string> loadout;
};

extern std::array<EntState, MAX_GENTITIES> ents;
extern MapRules rules;
extern std::array<ClientState, MAX_CLIENTS> clients;
extern Cvars mcv;

inline EntState& StateOf(const gentity_t* ent) { return ents[ent->s.number]; }

// mode.cpp
gentity_t* TempEvent(const vec3_t origin, int event, int eventParm = 0);  // an ET_EVENTS entity
void AddEvent(gentity_t* ent, int event, int eventParm);                 // on a client or arena entity
bool Damageable(const gentity_t* ent);
void RadiusDamage(const vec3_t origin, gentity_t* attacker, float damage, float radius, gentity_t* ignore, int mod,
                  float knockbackScale);
void GiveWeapon(playerState_t& ps, int slot, bool select);

// weapons_fire.cpp
void FireWeapon(gentity_t* player, playerState_t& ps);
void TacThink(playerState_t& ps);      // reloads, reserve on the HUD
bool StartReload(playerState_t& ps);   // false if there's nothing to reload
void RunMissile(gentity_t* ent);

// items.cpp
bool SpawnItem(int item, const EntityKeys& keys);
void TouchItem(gentity_t* ent, gentity_t* player);
void RunItem(gentity_t* ent);

// dummies.cpp
bool SpawnDummy(const EntityKeys& keys);
void RunDummy(gentity_t* ent);
void DamageDummy(gentity_t* ent, gentity_t* attacker, const vec3_t dir, int damage, int dflags, float knockbackScale);

// "x y z" from a map entity key
bool KeyVector(const EntityKeys& keys, const char* key, vec3_t out);

}  // namespace sg::mode
