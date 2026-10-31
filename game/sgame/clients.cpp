// Client lifecycle and movement on the server: connect, spawn, think (pmove).
// The think/spawn flow follows Q3's g_client.c / g_active.c, minus everything
// the aim trainer doesn't need yet (items, damage, teams, bots).
//
// This file is part of Skaar. GPLv2 or later.
#include <cstring>

#include "local.h"
#include "mode.h"
#include "shared/move_pmove.h"
#include "shared/move_ruleset.h"

namespace sg {
namespace {

const vec3_t kPlayerMins = {-15, -15, MINS_Z};
const vec3_t kPlayerMaxs = {15, 15, 32};

gentity_t& EntityFor(int clientNum) { return g_entities[clientNum]; }

void SendPrint(int clientNum, const char* text) { trap::SendServerCommand(clientNum, skaar::Va("print \"%s\"", text)); }

void SetViewAngles(gclient_t& client, gentity_t& ent, const vec3_t angles) {
    // The client's command angles are absolute; delta_angles makes them start at `angles`
    for (int i = 0; i < 3; i++) {
        client.ps.delta_angles[i] = ANGLE2SHORT(angles[i]) - client.cmd.angles[i];
    }
    VectorCopy(angles, ent.s.angles);
    VectorCopy(angles, client.ps.viewangles);
}

void Spawn(int clientNum) {
    gclient_t& client = g_clients[clientNum];
    gentity_t& ent = EntityFor(clientNum);

    const SpawnPoint& spot = level.spawns[level.nextSpawn++ % level.spawns.size()];
    vec3_t origin;
    VectorCopy(spot.origin, origin);
    origin[2] += 9;  // as Q3: spawn slightly above the marker so the box doesn't start in the floor

    // a fresh player state, but keep the bits the engine owns
    const int ping = client.ps.ping;
    const int eFlags = client.ps.eFlags;
    std::memset(&client.ps, 0, sizeof(client.ps));
    client.ps.ping = ping;
    client.ps.clientNum = clientNum;
    client.ps.eFlags = (eFlags & EF_TELEPORT_BIT) ^ EF_TELEPORT_BIT;  // no interpolation from the old spot
    client.ps.stats[STAT_HEALTH] = client.ps.stats[STAT_MAX_HEALTH] = 100;
    client.ps.pm_type = PM_NORMAL;
    client.ps.weapon = WP_NONE;
    client.ps.viewheight = DEFAULT_VIEWHEIGHT;
    VectorCopy(origin, client.ps.origin);
    // brief knockback lock so a held key doesn't move the player on the spawn frame
    client.ps.pm_flags |= PMF_TIME_KNOCKBACK;
    client.ps.pm_time = 100;
    client.ps.commandTime = level.time - 100;

    ent.s.number = clientNum;
    ent.s.eType = ET_PLAYER;
    ent.client = &client;
    ent.inuse = true;
    ent.classname = "player";
    ent.r.contents = CONTENTS_BODY;
    ent.r.svFlags = 0;
    // not ENTITYNUM_NONE here and traces skip us: the engine drops every entity whose
    // ownerNum equals the trace's pass or owner entity, and a player is nobody's owner
    ent.r.ownerNum = ENTITYNUM_NONE;
    VectorCopy(kPlayerMins, ent.r.mins);
    VectorCopy(kPlayerMaxs, ent.r.maxs);
    VectorCopy(origin, ent.r.currentOrigin);

    trap::GetUsercmd(clientNum, &client.cmd);
    SetViewAngles(client, ent, spot.angles);
    mode::ClientSpawn(clientNum, client.ps);

    client.spawned = true;
    BG_PlayerStateToEntityState(&client.ps, &ent.s, qtrue);
    trap::LinkEntity(&ent);

    Com_DPrintf("sgame: client %d spawned at (%.0f %.0f %.0f) yaw %.0f\n", clientNum, origin[0], origin[1],
                origin[2], spot.angles[YAW]);
}

bool CheatsAllowed(int clientNum) {
    if (cvars.cheats.integer) {
        return true;
    }
    SendPrint(clientNum, "Cheats are off on this server (start the map with devmap).\n");
    return false;
}

}  // namespace

const char* ClientConnect(int clientNum, bool firstTime, bool isBot) {
    if (isBot) {
        return "bots arrive in P6";
    }
    gclient_t& client = g_clients[clientNum];
    std::memset(&client, 0, sizeof(client));
    client.connected = true;
    ClientUserinfoChanged(clientNum);
    Com_DPrintf("sgame: client %d connected (%s)%s\n", clientNum, client.name, firstTime ? "" : ", reconnect");
    return nullptr;
}

void ClientBegin(int clientNum) {
    gentity_t& ent = EntityFor(clientNum);
    if (ent.r.linked) {
        trap::UnlinkEntity(&ent);
    }
    std::memset(&ent, 0, sizeof(ent));
    Spawn(clientNum);
}

void ClientUserinfoChanged(int clientNum) {
    gclient_t& client = g_clients[clientNum];
    const std::string info = trap::GetUserinfo(clientNum);
    char name[64];
    Q_strncpyz(name, Info_ValueForKey(info.c_str(), "name"), sizeof(name));
    Q_CleanStr(name);
    Q_strncpyz(client.name, name[0] ? name : "player", sizeof(client.name));
    trap::SetConfigstring(CS_PLAYERS + clientNum, skaar::Va("n\\%s", client.name));
}

void ClientDisconnect(int clientNum) {
    gentity_t& ent = EntityFor(clientNum);
    trap::UnlinkEntity(&ent);
    std::memset(&ent, 0, sizeof(ent));
    std::memset(&g_clients[clientNum], 0, sizeof(gclient_t));
    trap::SetConfigstring(CS_PLAYERS + clientNum, "");
    Com_DPrintf("sgame: client %d disconnected\n", clientNum);
}

void ClientCommand(int clientNum) {
    gclient_t& client = g_clients[clientNum];
    if (!client.spawned) {
        return;
    }
    const std::string cmd = trap::Argv(0);

    if (!Q_stricmp(cmd.c_str(), "noclip")) {
        if (CheatsAllowed(clientNum)) {
            client.noclip = !client.noclip;
            SendPrint(clientNum, client.noclip ? "noclip ON\n" : "noclip OFF\n");
        }
    } else if (!Q_stricmp(cmd.c_str(), "setviewpos")) {
        // setviewpos x y z [yaw]: teleport, for testing (cheat)
        if (CheatsAllowed(clientNum)) {
            if (trap::Argc() < 4) {
                SendPrint(clientNum, "usage: setviewpos x y z [yaw]\n");
            } else {
                gentity_t& ent = EntityFor(clientNum);
                vec3_t angles = {0, static_cast<float>(atof(trap::Argv(4).c_str())), 0};
                for (int i = 0; i < 3; i++) {
                    client.ps.origin[i] = static_cast<float>(atof(trap::Argv(i + 1).c_str()));
                }
                client.ps.origin[2] -= DEFAULT_VIEWHEIGHT;  // the arguments are the eye position, as viewpos prints
                VectorClear(client.ps.velocity);
                client.ps.eFlags ^= EF_TELEPORT_BIT;
                SetViewAngles(client, ent, angles);
                BG_PlayerStateToEntityState(&client.ps, &ent.s, qtrue);
                trap::LinkEntity(&ent);
            }
        }
    } else if (!Q_stricmp(cmd.c_str(), "sk_freeze")) {
        // sent by the aim trainer for scenarios where the player doesn't move
        client.frozen = atoi(trap::Argv(1).c_str()) != 0;
        Com_DPrintf("sgame: client %d %s\n", clientNum, client.frozen ? "frozen" : "unfrozen");
    } else if (!Q_stricmp(cmd.c_str(), "kill")) {
        if (!client.dead) {
            Kill(clientNum);
        }
    } else if (!Q_stricmp(cmd.c_str(), "respawn") || !Q_stricmp(cmd.c_str(), "sk_restart")) {
        // sk_restart reaches the server only when the cgame trainer didn't handle it
        // (no scenario loaded): fall back to a respawn
        Spawn(clientNum);
    } else if (!mode::ClientCommand(clientNum, cmd.c_str())) {
        SendPrint(clientNum, skaar::Va("unknown command: %s\n", cmd.c_str()));
    }
}

void Kill(int clientNum) {
    gclient_t& client = g_clients[clientNum];
    gentity_t& ent = EntityFor(clientNum);
    if (!client.spawned || client.dead) {
        return;
    }
    client.dead = true;
    client.respawnAllowedTime = level.time + 1000;
    client.respawnForcedTime = level.time + 3000;
    if (client.ps.stats[STAT_HEALTH] > 0) {
        client.ps.stats[STAT_HEALTH] = 0;
    }
    client.ps.pm_type = PM_DEAD;
    ent.r.contents = CONTENTS_CORPSE;
    trap::LinkEntity(&ent);
    Com_DPrintf("sgame: client %d died\n", clientNum);
}

void Respawn(int clientNum) {
    gclient_t& client = g_clients[clientNum];
    client.dead = false;
    Spawn(clientNum);
}

void ClientThink(int clientNum) {
    gclient_t& client = g_clients[clientNum];
    gentity_t& ent = EntityFor(clientNum);
    if (!client.spawned) {
        return;
    }

    usercmd_t ucmd;
    trap::GetUsercmd(clientNum, &ucmd);
    client.cmd = ucmd;

    // clamp the command time into a sane window around the server's clock (as Q3)
    if (ucmd.serverTime > level.time + 200) {
        ucmd.serverTime = level.time + 200;
    }
    if (ucmd.serverTime < level.time - 1000) {
        ucmd.serverTime = level.time - 1000;
    }
    if (ucmd.serverTime - client.ps.commandTime < 1) {
        return;
    }

    int msec = cvars.pmoveMsec.integer;
    if (msec < 8 || msec > 33) {
        msec = msec < 8 ? 8 : 33;
        trap::CvarSet("pmove_msec", skaar::Va("%i", msec));
    }
    if (cvars.pmoveFixed.integer) {
        ucmd.serverTime = ((ucmd.serverTime + msec - 1) / msec) * msec;
    }

    if (client.dead) {
        // respawn on attack/jump once allowed, or anyway after the forced delay
        const bool wants = (ucmd.buttons & BUTTON_ATTACK) || ucmd.upmove > 0;
        if (level.time >= client.respawnForcedTime || (wants && level.time >= client.respawnAllowedTime)) {
            Respawn(clientNum);
            return;
        }
    }

    client.ps.pm_type = client.dead     ? PM_DEAD
                        : client.noclip ? PM_NOCLIP
                        : client.frozen ? PM_FREEZE
                                        : PM_NORMAL;
    client.ps.gravity = cvars.gravity.integer;
    client.ps.speed = cvars.speed.integer;

    client.ps.stats[skaar::move::kStatRuleset] = level.ruleset;

    pmove_t pm{};
    pm.ps = &client.ps;
    pm.cmd = ucmd;
    pm.tracemask = MASK_PLAYERSOLID;
    pm.trace = trap::Trace;
    pm.pointcontents = trap::PointContents;
    pm.noFootsteps = qtrue;
    pm.pmove_fixed = cvars.pmoveFixed.integer;
    pm.pmove_msec = msec;
    const int oldEventSequence = client.ps.eventSequence;
    skaar::move::Pmove(&pm);

    BG_PlayerStateToEntityState(&client.ps, &ent.s, qtrue);
    VectorCopy(client.ps.origin, ent.r.currentOrigin);
    VectorCopy(pm.mins, ent.r.mins);
    VectorCopy(pm.maxs, ent.r.maxs);
    TouchTriggers(ent, client);
    mode::ClientThink(clientNum, ucmd, client.ps, oldEventSequence);
    BG_PlayerStateToEntityState(&client.ps, &ent.s, qtrue);
    trap::LinkEntity(&ent);
}

// Triggers the player's box overlaps after this move (jump pads for now)
void TouchTriggers(gentity_t& ent, gclient_t& client) {
    // jumppad_ent marks "already touching this pad" for the event; it lapses
    // once a move happens without touching it
    if (client.ps.jumppad_frame != client.ps.pmove_framecount) {
        client.ps.jumppad_ent = 0;
    }
    if (client.ps.pm_type != PM_NORMAL) {
        return;
    }

    vec3_t mins, maxs;
    VectorAdd(client.ps.origin, ent.r.mins, mins);
    VectorAdd(client.ps.origin, ent.r.maxs, maxs);

    int list[MAX_GENTITIES];
    const int n = trap::EntitiesInBox(mins, maxs, list, MAX_GENTITIES);
    for (int i = 0; i < n; i++) {
        gentity_t& other = g_entities[list[i]];
        if (!(other.r.contents & CONTENTS_TRIGGER) || !trap::EntityContact(mins, maxs, &other)) {
            continue;
        }
        if (other.s.eType == ET_PUSH_TRIGGER) {
            BG_TouchJumpPad(&client.ps, &other.s);
        } else {
            mode::Touch(&other, &ent);  // items, mode triggers
        }
    }
}

}  // namespace sg
