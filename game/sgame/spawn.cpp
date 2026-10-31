// Parse the map's entity lump: worldspawn, player spawn points, jump pads
// (trigger_push + target_position). Everything else is logged and ignored.
//
// This file is part of Skaar. GPLv2 or later.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "local.h"
#include "mode.h"

namespace sg {
namespace {

// Reads one { "key" "value" ... } block. Returns false at the end of the lump.
bool ReadEntity(EntityKeys& keys) {
    char token[MAX_TOKEN_CHARS];
    if (!trap::GetEntityToken(token, sizeof(token))) {
        return false;
    }
    if (token[0] != '{') {
        Com_Error(ERR_DROP, "sgame: entity lump: expected '{', got '%s'", token);
    }
    keys.clear();
    for (;;) {
        char key[MAX_TOKEN_CHARS];
        if (!trap::GetEntityToken(key, sizeof(key))) {
            Com_Error(ERR_DROP, "sgame: entity lump: unexpected end");
        }
        if (key[0] == '}') {
            return true;
        }
        if (!trap::GetEntityToken(token, sizeof(token))) {
            Com_Error(ERR_DROP, "sgame: entity lump: key '%s' has no value", key);
        }
        keys[key] = token;
    }
}

void ReadVector(const EntityKeys& keys, const char* key, vec3_t out) {
    VectorClear(out);
    auto it = keys.find(key);
    if (it != keys.end()) {
        std::sscanf(it->second.c_str(), "%f %f %f", &out[0], &out[1], &out[2]);
        ClampCoords(out);
    }
}

struct PendingPush {
    gentity_t* ent;
    std::string target;
};

std::map<std::string, std::array<float, 3>> g_targets;  // target_position origins by targetname
std::vector<PendingPush> g_pushes;

// trigger_push: a brush that launches the player towards its target's
// position (the apex of the arc), as Q3's jump pads
void SpawnTriggerPush(EntityKeys& keys) {
    if (!keys.count("model") || keys["model"][0] != '*') {
        Com_Printf(S_COLOR_YELLOW "WARNING: trigger_push without a brush model, ignored\n");
        return;
    }
    gentity_t* ent = Spawn();
    ent->classname = "trigger_push";
    trap::SetBrushModel(ent, keys["model"].c_str());
    ent->r.contents = CONTENTS_TRIGGER;
    ent->r.svFlags = 0;               // sent to clients so prediction can apply it too
    ent->s.eType = ET_PUSH_TRIGGER;
    trap::LinkEntity(ent);
    g_pushes.push_back({ent, keys.count("target") ? keys["target"] : ""});
}

// Launch velocity that peaks exactly at the target (Q3's AimAtTarget)
void AimPushes() {
    const float gravity = cvars.gravity.value > 0 ? cvars.gravity.value : 800.0f;
    for (const PendingPush& push : g_pushes) {
        gentity_t* ent = push.ent;
        auto target = g_targets.find(push.target);
        if (target == g_targets.end()) {
            Com_Printf(S_COLOR_YELLOW "WARNING: trigger_push target '%s' not found, removed\n", push.target.c_str());
            FreeEntity(ent);
            continue;
        }
        vec3_t origin, dir;
        VectorAdd(ent->r.absmin, ent->r.absmax, origin);
        VectorScale(origin, 0.5f, origin);
        const float height = target->second[2] - origin[2];
        if (height <= 0) {
            Com_Printf(S_COLOR_YELLOW "WARNING: trigger_push target '%s' is below the pad, removed\n",
                       push.target.c_str());
            FreeEntity(ent);
            continue;
        }
        const float time = std::sqrt(height / (0.5f * gravity));
        dir[0] = target->second[0] - origin[0];
        dir[1] = target->second[1] - origin[1];
        dir[2] = 0;
        const float dist = VectorNormalize(dir);
        VectorScale(dir, dist / time, ent->s.origin2);
        ent->s.origin2[2] = time * gravity;
        trap::LinkEntity(ent);
        Com_DPrintf("sgame: jump pad %d -> '%s': %.0f ups up, %.0f ups across\n", ent->s.number,
                    push.target.c_str(), ent->s.origin2[2], dist / time);
    }
    g_pushes.clear();
}

void SpawnPlayerStart(const EntityKeys& keys) {
    SpawnPoint spot{};
    ReadVector(keys, "origin", spot.origin);
    auto angle = keys.find("angle");
    if (angle != keys.end()) {
        // wrapping at 360 keeps the value small enough for ANGLE2SHORT's int conversion
        const float yaw = static_cast<float>(std::atof(angle->second.c_str()));
        spot.angles[YAW] = std::isfinite(yaw) ? std::fmod(yaw, 360.0f) : 0;
    }
    level.spawns.push_back(spot);
}

}  // namespace

gentity_t* Spawn() {
    for (int i = MAX_CLIENTS; i < level.numEntities; i++) {
        if (!g_entities[i].inuse) {
            g_entities[i] = gentity_t{};
            g_entities[i].s.number = i;
            g_entities[i].inuse = true;
            return &g_entities[i];
        }
    }
    if (level.numEntities >= ENTITYNUM_MAX_NORMAL) {
        Com_Error(ERR_DROP, "sgame: no free entities");
    }
    gentity_t* ent = &g_entities[level.numEntities++];
    *ent = gentity_t{};
    ent->s.number = static_cast<int>(ent - g_entities.data());
    ent->inuse = true;
    LinkGameData();  // the engine needs the new entity count
    return ent;
}

void FreeEntity(gentity_t* ent) {
    trap::UnlinkEntity(ent);
    *ent = gentity_t{};
}

void SpawnEntitiesFromString() {
    EntityKeys keys;
    g_targets.clear();
    g_pushes.clear();
    bool first = true;
    while (ReadEntity(keys)) {
        const std::string classname = keys.count("classname") ? keys["classname"] : "";
        if (first) {
            if (classname != "worldspawn") {
                Com_Error(ERR_DROP, "sgame: the first entity isn't worldspawn");
            }
            if (keys.count("message")) {
                trap::SetConfigstring(CS_MESSAGE, keys["message"].c_str());
            }
            first = false;
        } else if (classname == "info_player_deathmatch" || classname == "info_player_start") {
            SpawnPlayerStart(keys);
        } else if (classname == "trigger_push") {
            SpawnTriggerPush(keys);
        } else if (classname == "target_position" || classname == "info_notnull") {
            vec3_t origin;
            ReadVector(keys, "origin", origin);
            if (keys.count("targetname")) {
                g_targets[keys["targetname"]] = {origin[0], origin[1], origin[2]};
            }
        } else if (!mode::SpawnEntity(classname.c_str(), keys)) {
            Com_DPrintf("sgame: ignoring entity '%s'\n", classname.c_str());
        }
    }

    AimPushes();

    if (level.spawns.empty()) {
        Com_Printf(S_COLOR_YELLOW "WARNING: map %s has no info_player_deathmatch; spawning at the origin\n",
                   level.mapname.c_str());
        level.spawns.push_back(SpawnPoint{});
    }
}

}  // namespace sg
