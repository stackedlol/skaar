// Server game state.
//
// This file is part of Skaar. GPLv2 or later.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "syscalls.h"

namespace sg {

struct gclient_t;

using EntityKeys = std::map<std::string, std::string>;  // a map entity's key/value pairs

// Entity coordinates come from the map's entity lump (mapc writes JSON numbers into it
// verbatim), and they end up in int conversions: SnapVector truncates the origin,
// ANGLE2SHORT converts the yaw. Both are UB outside int's range, so a value the map cannot
// legitimately mean is clamped away where it is read.
constexpr float kMaxCoord = 65535;  // MAX_MAP_BOUNDS (cm_polylib.h), far past any real map

inline void ClampCoords(vec3_t v) {
    for (int i = 0; i < 3; i++) {
        v[i] = std::isfinite(v[i]) ? std::clamp(v[i], -kMaxCoord, kMaxCoord) : 0;
    }
}

// The engine reads the leading entityState_t + entityShared_t of every
// entity (sharedEntity_t), so those two must stay first and in this order.
struct gentity_t {
    entityState_t s;
    entityShared_t r;

    gclient_t* client;
    bool inuse;
    const char* classname;
};

// Likewise the engine reads playerState_t at the start of each client.
struct gclient_t {
    playerState_t ps;

    bool connected;
    bool spawned;
    bool noclip;
    bool frozen;    // sk_freeze: position locked, view still turns (static scenarios)
    bool dead;
    int respawnAllowedTime;  // level.time after which attack/jump respawns
    int respawnForcedTime;   // level.time at which respawn happens anyway
    usercmd_t cmd;  // last command, for view angle deltas on spawn
    char name[36];
};

struct SpawnPoint {
    vec3_t origin;
    vec3_t angles;
};

struct Level {
    int time;
    int previousTime;
    int startTime;
    int numEntities;  // highest used entity number + 1
    std::string mapname;
    std::vector<SpawnPoint> spawns;
    size_t nextSpawn;
    int ruleset;  // movement ruleset index (game/shared/move_ruleset.h)
};

struct Cvars {
    vmCvar_t speed, gravity, pmoveFixed, pmoveMsec, cheats, movement;
};

extern std::array<gentity_t, MAX_GENTITIES> g_entities;
extern std::array<gclient_t, MAX_CLIENTS> g_clients;
extern Level level;
extern Cvars cvars;

// spawn.cpp
void SpawnEntitiesFromString();
gentity_t* Spawn();               // a free non-client entity (Com_Error if the level is full)
void FreeEntity(gentity_t* ent);  // unlink and release

// clients.cpp (triggers, life cycle)
void TouchTriggers(gentity_t& ent, gclient_t& client);
void Kill(int clientNum);     // health <= 0: PM_DEAD, respawn on attack/jump after 1 s, forced after 3 s
void Respawn(int clientNum);  // immediately, at the next spawn point

// clients.cpp
const char* ClientConnect(int clientNum, bool firstTime, bool isBot);
void ClientBegin(int clientNum);
void ClientUserinfoChanged(int clientNum);
void ClientDisconnect(int clientNum);
void ClientCommand(int clientNum);
void ClientThink(int clientNum);

// main.cpp
void LinkGameData();

}  // namespace sg
