// Arena mode (P6): the hooks the server loop calls, damage, events, loadouts.
// Weapons fire in weapons_fire.cpp, items in items.cpp, dummies in dummies.cpp.
// Damage and knockback follow Q3's g_combat.c (GPL).
//
// This file is part of Skaar. GPLv2 or later.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "mode_local.h"
#include "shared/arenalog.h"
#include "shared/move_pmove.h"
#include "shared/move_ruleset.h"

namespace sg::mode {

std::array<EntState, MAX_GENTITIES> ents;
std::array<ClientState, MAX_CLIENTS> clients;
Cvars mcv;
MapRules rules;

namespace {

constexpr int kDamageRadius = 0x1;       // Q3 DAMAGE_* flags (g_local.h)
constexpr int kDamageNoKnockback = 0x4;
constexpr float kArmorProtection = 0.66f;
// damage is an int the game does arithmetic on; anything past this is nonsense data, and
// 2147483647 itself is not representable as a float, so the clamp value stays well inside int
constexpr float kMaxDamage = 1.0e9f;
// knockback scales the whole push (WeaponDef::knockback); the weapon parser accepts 0..100,
// so the cap here has to be as wide or a "knockback": 50 pushes at a tenth of its value
constexpr float kMaxKnockbackScale = 100.0f;

void LoadWeapons() {
    skaar::ResetWeapons();
    for (const std::string& file : trap::FileList("weapons", ".json")) {
        const std::string id = file.substr(0, file.size() - 5);
        std::string text, error;
        skaar::WeaponDef w;
        if (!trap::FileRead(skaar::Va("weapons/%s", file.c_str()), text)) {
            continue;
        }
        if (skaar::ParseWeapon(id, text, w, error)) {
            skaar::AddWeapon(w);
        } else {
            Com_Printf(S_COLOR_YELLOW "WARNING: weapons/%s: %s\n", file.c_str(), error.c_str());
        }
    }
    // refire times from the definitions; the cgame sets the same so prediction agrees
    skaar::move::ClearWeaponRefire();
    skaar::move::ClearWeaponSemiAuto();
    for (const skaar::WeaponDef& w : skaar::Weapons()) {
        skaar::move::SetWeaponRefire(w.slot, w.refireMs);
        skaar::move::SetWeaponSemiAuto(w.slot, w.semiAuto);
    }
}

// Free temp events, clear stale entity events, run missiles, items and dummies
void RunEntities() {
    for (int i = MAX_CLIENTS; i < level.numEntities; i++) {
        gentity_t* ent = &g_entities[i];
        EntState& st = ents[i];
        if (!ent->inuse || st.kind == EntState::Kind::kNone) {
            continue;
        }
        if (st.eventTime && level.time - st.eventTime > EVENT_VALID_MSEC) {
            ent->s.event = 0;
            ent->s.eventParm = 0;
            st.eventTime = 0;
        }
        switch (st.kind) {
            case EntState::Kind::kTemp:
                if (level.time >= st.freeAt) {
                    st = EntState{};
                    FreeEntity(ent);
                }
                break;
            case EntState::Kind::kMissile:
                RunMissile(ent);
                break;
            case EntState::Kind::kItem:
                RunItem(ent);
                break;
            case EntState::Kind::kDummy:
                RunDummy(ent);
                break;
            default:
                break;
        }
    }
}

// Health and armor above 100 tick down once a second (Q3's mega/armor decay)
void Decay() {
    for (int c = 0; c < MAX_CLIENTS; c++) {
        gclient_t& cl = g_clients[c];
        if (!cl.spawned || cl.dead) {
            continue;
        }
        ClientState& cs = clients[c];
        if (level.time - cs.lastDecay < 1000) {
            continue;
        }
        cs.lastDecay = level.time;
        if (cl.ps.stats[STAT_HEALTH] > skaar::arena::kMaxHealth) {
            cl.ps.stats[STAT_HEALTH]--;
        }
        if (cl.ps.stats[STAT_ARMOR] > skaar::arena::kMaxHealth) {
            cl.ps.stats[STAT_ARMOR]--;
        }
        // Q3 clears a client's external event once it's old
        if (cl.ps.externalEvent && level.time - cl.ps.externalEventTime > EVENT_VALID_MSEC) {
            cl.ps.externalEvent = 0;
            cl.ps.externalEventParm = 0;
        }
    }
}

// One stats/arena.jsonl line for a client's session, if they fired at all
void WriteSession(int clientNum) {
    ClientState& cs = clients[clientNum];
    if (!cs.inSession) {
        return;
    }
    skaar::ArenaSession s;
    qtime_t t{};
    trap::RealTime(&t);
    s.when = {t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec};
    s.map = level.mapname;
    s.ruleset = skaar::move::RulesetAt(level.ruleset).id;  // what ran, not what g_movement asked for
    s.seconds = (level.time - cs.sessionStart) / 1000.0f;
    s.damageDealt = cs.damageDealt;
    s.damageTaken = cs.damageTaken;
    s.kills = cs.kills;
    s.deaths = cs.deaths;
    int shots = 0;
    for (const skaar::WeaponDef& w : skaar::Weapons()) {
        if (cs.shots[w.slot]) {
            s.weapons[w.id] = {cs.shots[w.slot], cs.hits[w.slot]};
            shots += cs.shots[w.slot];
        }
    }
    if (shots > 0 && !trap::FileWrite(skaar::kArenaHistoryPath, skaar::ArenaSessionToJsonLine(s) + "\n", true)) {
        Com_Printf(S_COLOR_YELLOW "WARNING: could not append %s\n", skaar::kArenaHistoryPath);
    }
    cs = ClientState{};
}

}  // namespace

// ---- helpers ------------------------------------------------------------------------

bool KeyVector(const EntityKeys& keys, const char* key, vec3_t out) {
    auto it = keys.find(key);
    if (it == keys.end()) {
        return false;
    }
    std::istringstream in(it->second);
    if (!(in >> out[0] >> out[1] >> out[2])) {
        return false;
    }
    // items and dummies are linked and snapped like the spawn points: a "1e30 0 40" origin
    // is UB in the entity-delta writer and in the client's lightgrid lookup
    ClampCoords(out);
    return true;
}

gentity_t* TempEvent(const vec3_t origin, int event, int eventParm) {
    gentity_t* e = Spawn();
    e->classname = "tempEntity";
    e->s.eType = ET_EVENTS + event;
    e->s.eventParm = eventParm;
    e->s.pos.trType = TR_STATIONARY;
    VectorCopy(origin, e->s.pos.trBase);
    VectorCopy(origin, e->r.currentOrigin);
    EntState& st = StateOf(e);
    st = EntState{};
    st.kind = EntState::Kind::kTemp;
    st.freeAt = level.time + EVENT_VALID_MSEC;
    trap::LinkEntity(e);
    return e;
}

void AddEvent(gentity_t* ent, int event, int eventParm) {
    // the two event bits make a repeat of the same event look new (Q3 G_AddEvent)
    if (ent->client) {
        playerState_t& ps = ent->client->ps;
        const int bits = ((ps.externalEvent & EV_EVENT_BITS) + EV_EVENT_BIT1) & EV_EVENT_BITS;
        ps.externalEvent = event | bits;
        ps.externalEventParm = eventParm;
        ps.externalEventTime = level.time;
    } else {
        const int bits = ((ent->s.event & EV_EVENT_BITS) + EV_EVENT_BIT1) & EV_EVENT_BITS;
        ent->s.event = event | bits;
        ent->s.eventParm = eventParm;
        StateOf(ent).eventTime = level.time;
    }
}

bool Damageable(const gentity_t* ent) {
    if (!ent || !ent->inuse) {
        return false;
    }
    if (ent->client) {
        return ent->client->spawned && !ent->client->dead;
    }
    const EntState& st = ents[ent->s.number];
    return st.kind == EntState::Kind::kDummy && st.deadUntil == 0;
}

void RadiusDamage(const vec3_t origin, gentity_t* attacker, float damage, float radius, gentity_t* ignore, int mod,
                  float knockbackScale) {
    if (radius < 1) {
        return;
    }
    vec3_t mins, maxs;
    for (int i = 0; i < 3; i++) {
        mins[i] = origin[i] - radius;
        maxs[i] = origin[i] + radius;
    }
    int list[MAX_GENTITIES];
    const int n = trap::EntitiesInBox(mins, maxs, list, MAX_GENTITIES);
    for (int k = 0; k < n; k++) {
        gentity_t* ent = &g_entities[list[k]];
        if (ent == ignore || !Damageable(ent)) {
            continue;
        }
        // distance from the edge of the target's box
        vec3_t v;
        for (int i = 0; i < 3; i++) {
            v[i] = origin[i] < ent->r.absmin[i]   ? ent->r.absmin[i] - origin[i]
                   : origin[i] > ent->r.absmax[i] ? origin[i] - ent->r.absmax[i]
                                                  : 0;
        }
        const float dist = VectorLength(v);
        if (dist >= radius) {
            continue;
        }
        // only through open space (Q3 CanDamage, centre test)
        vec3_t centre;
        VectorAdd(ent->r.absmin, ent->r.absmax, centre);
        VectorScale(centre, 0.5f, centre);
        trace_t tr;
        trap::Trace(&tr, origin, nullptr, nullptr, centre, ENTITYNUM_NONE, MASK_SOLID);
        if (tr.fraction < 1.0f) {
            continue;
        }
        vec3_t dir;
        VectorSubtract(ent->r.currentOrigin, origin, dir);
        dir[2] += 24;  // push the centre of mass higher, so players go up more
        // the weapon parser bounds splashDamage only from below, and a float->int conversion
        // outside int's range is UB (INT_MIN here, INT_MAX there), so saturate first
        const float scaled = damage * (1.0f - dist / radius);
        const int take = static_cast<int>(std::clamp(std::isfinite(scaled) ? scaled : 0.0f, 0.0f, kMaxDamage));
        Damage(ent, nullptr, attacker, dir, origin, take, kDamageRadius, mod, knockbackScale);
    }
}

void GiveWeapon(playerState_t& ps, int slot, bool select) {
    const skaar::WeaponDef* w = skaar::WeaponBySlot(slot);
    if (!w) {
        return;
    }
    const bool had = ps.stats[STAT_WEAPONS] & (1 << slot);
    ps.stats[STAT_WEAPONS] |= 1 << slot;
    if (w->magazine > 0) {
        // tac weapons: a full magazine, the rest in reserve (pmove counts the magazine down)
        int& reserve = clients[ps.clientNum].reserve[slot];
        if (!had) {
            ps.ammo[slot] = w->magazine;
            reserve = std::max(reserve, w->ammoStart);
        } else {
            reserve = std::min(std::max(reserve, w->ammoStart), w->ammoMax);
        }
    } else if (mcv.infiniteAmmo.integer) {
        ps.ammo[slot] = -1;  // pmove never counts down -1
    } else if (ps.ammo[slot] >= 0) {
        ps.ammo[slot] = std::min(std::max(ps.ammo[slot], w->ammoStart), w->ammoMax);
    }
    if (select || ps.weapon == WP_NONE) {
        ps.weapon = slot;
        ps.weaponstate = WEAPON_READY;
    }
}

// ---- hooks ----------------------------------------------------------------------------

void Init() {
    ents.fill(EntState{});
    clients.fill(ClientState{});
    rules = MapRules{};
    trap::CvarRegister(&mcv.knockback, "g_knockback", "1000", 0);
    trap::CvarRegister(&mcv.infiniteAmmo, "g_infiniteAmmo", "0", CVAR_ARCHIVE);
    trap::CvarRegister(&mcv.loadout, "g_loadout", "1", CVAR_ARCHIVE);
    LoadWeapons();
}

void Shutdown() {
    for (int c = 0; c < MAX_CLIENTS; c++) {
        WriteSession(c);
    }
}

void RunFrame(int /*levelTime*/) {
    for (vmCvar_t* c : {&mcv.knockback, &mcv.infiniteAmmo, &mcv.loadout}) {
        trap::CvarUpdate(c);
    }
    RunEntities();
    Decay();
}

void ClientDisconnect(int clientNum) {
    // the slot is about to be handed to the next player, and WriteSession clears it:
    // without this the newcomer inherited these stats and the old sessionStart
    WriteSession(clientNum);
}

void ClientSpawn(int clientNum, playerState_t& ps) {
    ps.stats[STAT_HEALTH] = skaar::arena::kMaxHealth;
    ps.stats[STAT_MAX_HEALTH] = skaar::arena::kMaxHealth;
    ps.stats[STAT_ARMOR] = 0;
    ps.stats[STAT_WEAPONS] = 0;
    for (int& a : ps.ammo) {
        a = 0;
    }
    // loadouts only on arena maps (items, dummies or an info_loadout): the trainer's maps stay weaponless
    bool arenaMap = !rules.loadout.empty();
    for (int i = MAX_CLIENTS; i < level.numEntities && !arenaMap; i++) {
        arenaMap = ents[i].kind == EntState::Kind::kItem || ents[i].kind == EntState::Kind::kDummy;
    }
    clients[clientNum].reserve.fill(0);
    if (!rules.loadout.empty()) {
        // the map's own loadout, first one in hand
        for (const std::string& id : rules.loadout) {
            if (const skaar::WeaponDef* w = skaar::WeaponById(id)) {
                GiveWeapon(ps, w->slot, false);
            }
        }
        if (const skaar::WeaponDef* first = skaar::WeaponById(rules.loadout.front())) {
            ps.weapon = first->slot;
        }
    } else if (arenaMap && mcv.loadout.integer) {
        // arena practice: every arena weapon (not the tac ones, which have magazines)
        for (const skaar::WeaponDef& w : skaar::Weapons()) {
            if (w.magazine == 0) {
                GiveWeapon(ps, w.slot, false);
            }
        }
        if (const skaar::WeaponDef* rail = skaar::WeaponById("rail")) {
            ps.weapon = rail->slot;
        }
    }
    ClientState& cs = clients[clientNum];
    cs.lastDecay = level.time;
    if (arenaMap && !cs.inSession) {
        cs.inSession = true;
        cs.sessionStart = level.time;
    }
}

void ClientThink(int clientNum, const usercmd_t& /*cmd*/, playerState_t& ps, int oldEventSequence) {
    gentity_t* player = &g_entities[clientNum];
    if (oldEventSequence < ps.eventSequence - MAX_PS_EVENTS) {
        oldEventSequence = ps.eventSequence - MAX_PS_EVENTS;
    }
    for (int i = oldEventSequence; i < ps.eventSequence; i++) {
        const int event = ps.events[i & (MAX_PS_EVENTS - 1)] & ~EV_EVENT_BITS;
        if (event == EV_FIRE_WEAPON) {
            FireWeapon(player, ps);
        }
    }
    TacThink(ps);
}

bool ClientCommand(int clientNum, const char* cmd) {
    playerState_t& ps = g_clients[clientNum].ps;
    if (!Q_stricmp(cmd, "give")) {
        // give all | <weapon id>  (practice convenience)
        const std::string what = trap::Argv(1);
        for (const skaar::WeaponDef& w : skaar::Weapons()) {
            if (what == "all" || what == w.id) {
                GiveWeapon(ps, w.slot, what != "all");
            }
        }
        if (what == "all") {
            ps.stats[STAT_HEALTH] = 200;
            ps.stats[STAT_ARMOR] = 200;
        }
        return true;
    }
    if (!Q_stricmp(cmd, "reload")) {
        StartReload(ps);
        return true;
    }
    if (!Q_stricmp(cmd, "arenastats")) {
        const ClientState& cs = clients[clientNum];
        std::string text = skaar::Va("damage dealt %d, taken %d, kills %d, deaths %d\n", cs.damageDealt,
                                     cs.damageTaken, cs.kills, cs.deaths);
        for (const skaar::WeaponDef& w : skaar::Weapons()) {
            if (cs.shots[w.slot]) {
                text += skaar::Va("%-10s %d/%d  %.0f pct\n", w.name.c_str(), cs.hits[w.slot], cs.shots[w.slot],
                                  100.0f * cs.hits[w.slot] / cs.shots[w.slot]);
            }
        }
        trap::SendServerCommand(clientNum, skaar::Va("print \"%s\"", text.c_str()));
        return true;
    }
    return false;
}

bool SpawnEntity(const char* classname, const EntityKeys& keys) {
    const int item = skaar::arena::FindItem(classname);
    if (item >= 0) {
        return SpawnItem(item, keys);
    }
    if (!Q_stricmp(classname, "info_dummy")) {
        return SpawnDummy(keys);
    }
    if (!Q_stricmp(classname, "info_loadout")) {
        // "weapons": ids given on spawn; "ruleset": the movement physics for this map
        if (auto it = keys.find("weapons"); it != keys.end()) {
            std::istringstream in(it->second);
            std::string id;
            while (in >> id) {
                rules.loadout.push_back(id);
            }
        }
        if (auto it = keys.find("ruleset"); it != keys.end()) {
            // the registry is the only authority: LoadRulesets ran before the entity lump, so
            // an id that is merely a file on disk is one that failed to parse. Accepting it
            // would make g_movement point at a ruleset that cannot run (vq3 is silently
            // substituted) and archive that name into the player's config.
            if (skaar::move::FindRuleset(it->second) >= 0) {
                trap::CvarSet("g_movement", it->second.c_str());
            } else {
                Com_Printf(S_COLOR_YELLOW "WARNING: info_loadout: unknown ruleset '%s'\n", it->second.c_str());
            }
        }
        return true;
    }
    return false;
}

void Touch(gentity_t* trigger, gentity_t* player) {
    if (StateOf(trigger).kind == EntState::Kind::kItem) {
        TouchItem(trigger, player);
    }
}

void Damage(gentity_t* target, gentity_t* inflictor, gentity_t* attacker, const vec3_t dir, const vec3_t point,
            int damage, int dflags, int mod, float knockbackScale) {
    (void)inflictor;
    (void)point;
    (void)mod;
    if (!Damageable(target) || damage <= 0) {
        return;
    }
    // a per-weapon scale is bounded by the parser (0..100); a non-finite or huge one would
    // land in ps.velocity (then in the snapped entity origin), so bound it here too
    knockbackScale = std::isfinite(knockbackScale) ? std::clamp(knockbackScale, 0.0f, kMaxKnockbackScale) : 1.0f;
    if (!target->client) {
        DamageDummy(target, attacker, dir, damage, dflags, knockbackScale);
        return;
    }

    gclient_t& cl = *target->client;
    playerState_t& ps = cl.ps;
    const int targetNum = target->s.number;

    // knockback first, so rocket jumps get the full push (Q3)
    vec3_t d = {0, 0, 1};
    if (dir) {
        VectorCopy(dir, d);
        VectorNormalize(d);
    }
    int knockback = std::min(damage, 200);
    if (dflags & kDamageNoKnockback) {
        knockback = 0;
    }
    if (knockback && ps.pm_type == PM_NORMAL) {
        vec3_t kvel;
        VectorScale(d, mcv.knockback.value * knockbackScale * static_cast<float>(knockback) / 200.0f, kvel);
        VectorAdd(ps.velocity, kvel, ps.velocity);
        if (!ps.pm_time) {
            ps.pm_time = std::clamp(knockback * 2, 50, 200);
            ps.pm_flags |= PMF_TIME_KNOCKBACK;
        }
    }

    const bool selfHit = attacker == target;
    if (attacker && attacker->client && !selfHit) {
        attacker->client->ps.persistant[PERS_HITS]++;  // the cgame beeps on change
        clients[attacker->s.number].damageDealt += damage;
    }
    if (selfHit) {
        damage = static_cast<int>(damage * 0.5f);  // always half on yourself
    }
    damage = std::max(damage, 1);

    int save = static_cast<int>(std::ceil(damage * kArmorProtection));
    save = std::min(save, ps.stats[STAT_ARMOR]);
    ps.stats[STAT_ARMOR] -= save;
    const int take = damage - save;
    ps.stats[STAT_HEALTH] -= take;
    clients[targetNum].damageTaken += take;
    AddEvent(target, EV_PAIN, std::max(0, ps.stats[STAT_HEALTH]));

    if (ps.stats[STAT_HEALTH] <= 0) {
        clients[targetNum].deaths++;
        if (attacker && attacker->client && !selfHit) {
            clients[attacker->s.number].kills++;
        }
        Kill(targetNum);
    }
}

}  // namespace sg::mode
