// Arena items (P6): weapons, ammo, health, armor. Picked up by touch, then
// hidden until they respawn. Amounts and caps follow Q3 (game/shared/arena.h).
//
// This file is part of Skaar. GPLv2 or later.
#include <algorithm>

#include "mode_local.h"

namespace sg::mode {

namespace arena = skaar::arena;

bool SpawnItem(int item, const EntityKeys& keys) {
    vec3_t origin;
    if (!KeyVector(keys, "origin", origin)) {
        Com_Printf(S_COLOR_YELLOW "WARNING: %s without an origin\n", arena::kItems[item].classname);
        return true;  // handled (ignored), not an unknown entity
    }
    gentity_t* e = Spawn();
    e->classname = arena::kItems[item].classname;
    e->s.eType = ET_ITEM;
    e->s.generic1 = arena::kKindItem;
    e->s.modelindex = item;
    e->s.weapon = arena::kItems[item].slot;
    e->s.pos.trType = TR_STATIONARY;
    VectorCopy(origin, e->s.pos.trBase);
    VectorCopy(origin, e->r.currentOrigin);
    VectorSet(e->r.mins, -15, -15, -15);
    VectorSet(e->r.maxs, 15, 15, 15);
    e->r.contents = CONTENTS_TRIGGER;

    EntState& st = StateOf(e);
    st = EntState{};
    st.kind = EntState::Kind::kItem;
    st.item = item;
    trap::LinkEntity(e);
    return true;
}

void TouchItem(gentity_t* e, gentity_t* player) {
    EntState& st = StateOf(e);
    if (st.respawnAt || !player->client || player->client->dead) {
        return;
    }
    playerState_t& ps = player->client->ps;
    const arena::ItemDef& def = arena::kItems[st.item];
    bool taken = false;

    switch (def.type) {
        case arena::ItemType::kWeapon: {
            const bool had = ps.stats[STAT_WEAPONS] & (1 << def.slot);
            const int ammoBefore = ps.ammo[def.slot];
            // a magazine weapon keeps the magazine in ps.ammo and the rest in the reserve, and
            // touching it again only tops the reserve up: without that comparison a full
            // magazine made the pickup untouched and re-usable for free reserve ammo
            int& reserve = clients[player->s.number].reserve[def.slot];
            const int reserveBefore = reserve;
            GiveWeapon(ps, def.slot, false);
            taken = !had || ps.ammo[def.slot] != ammoBefore || reserve != reserveBefore;
            break;
        }
        case arena::ItemType::kAmmo: {
            const skaar::WeaponDef* w = skaar::WeaponBySlot(def.slot);
            if (w && w->magazine > 0) {
                // tac weapons: ammo goes to the reserve
                int& reserve = clients[player->s.number].reserve[def.slot];
                if (reserve < w->ammoMax) {
                    reserve = std::min(reserve + (def.amount ? def.amount : w->ammoPickup), w->ammoMax);
                    taken = true;
                }
            } else if (w && ps.ammo[def.slot] >= 0 && ps.ammo[def.slot] < w->ammoMax) {
                ps.ammo[def.slot] = std::min(ps.ammo[def.slot] + (def.amount ? def.amount : w->ammoPickup), w->ammoMax);
                taken = true;
            }
            break;
        }
        case arena::ItemType::kHealth:
            if (ps.stats[STAT_HEALTH] < def.max) {
                ps.stats[STAT_HEALTH] = std::min(ps.stats[STAT_HEALTH] + def.amount, def.max);
                taken = true;
            }
            break;
        case arena::ItemType::kArmor:
            if (ps.stats[STAT_ARMOR] < def.max) {
                ps.stats[STAT_ARMOR] = std::min(ps.stats[STAT_ARMOR] + def.amount, def.max);
                taken = true;
            }
            break;
    }
    if (!taken) {
        return;
    }
    AddEvent(player, EV_ITEM_PICKUP, st.item);
    st.respawnAt = level.time + def.respawnMs;
    e->s.eFlags |= EF_NODRAW;
    e->r.contents = 0;
    trap::LinkEntity(e);
}

void RunItem(gentity_t* e) {
    EntState& st = StateOf(e);
    if (!st.respawnAt || level.time < st.respawnAt) {
        return;
    }
    st.respawnAt = 0;
    e->s.eFlags &= ~EF_NODRAW;
    e->r.contents = CONTENTS_TRIGGER;
    AddEvent(e, EV_ITEM_RESPAWN, 0);
    trap::LinkEntity(e);
}

}  // namespace sg::mode
