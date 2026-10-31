// Game-mode hooks in the server loop (P6 arena: weapons, items, damage,
// dummies). Stubs here; the mode implementation lives in mode.cpp and
// weapons*.cpp (owned by the P6 plan, docs/plan-P6.md).
//
// This file is part of Skaar. GPLv2 or later.
#pragma once

#include <map>
#include <string>

#include "local.h"

namespace sg::mode {

void Init();      // before the map's entities spawn
void Shutdown();
void RunFrame(int levelTime);  // after the level clock advanced

// The client is gone: save their session and clear the per-client state, so the next
// player in the same slot starts clean.
void ClientDisconnect(int clientNum);

// After the base spawn set up ps (origin, angles, health 100); give loadouts here.
void ClientSpawn(int clientNum, playerState_t& ps);

// After Pmove and jump pads for one user command. ps.events between
// oldEventSequence and ps.eventSequence are the ones this move generated
// (EV_FIRE_WEAPON etc.), as Q3's ClientEvents walks them.
void ClientThink(int clientNum, const usercmd_t& cmd, playerState_t& ps, int oldEventSequence);

// Client commands the base game doesn't know; true if handled.
bool ClientCommand(int clientNum, const char* cmd);

// Map entities the base game doesn't know (weapon_*, item_*, ...); true if handled.
bool SpawnEntity(const char* classname, const EntityKeys& keys);

// A player's box overlaps a trigger entity (r.contents & CONTENTS_TRIGGER)
// that isn't a jump pad. Called once per move while overlapping.
void Touch(gentity_t* trigger, gentity_t* player);

// The single damage entry point. mod = means of death (bg_public.h);
// knockbackScale = the firing weapon's WeaponDef::knockback.
void Damage(gentity_t* target, gentity_t* inflictor, gentity_t* attacker, const vec3_t dir, const vec3_t point,
            int damage, int dflags, int mod, float knockbackScale);

}  // namespace sg::mode
