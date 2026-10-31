// Practice dummies (P6): player-sized targets that stand, strafe, strafe-jump
// or circle near their spawn, take damage and knockback, and respawn. Not
// bots: no pathing, no shooting (botlib needs .aas files mapc doesn't make).
//
// This file is part of Skaar. GPLv2 or later.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "mode_local.h"

namespace sg::mode {
namespace {

namespace arena = skaar::arena;

const vec3_t kMins = {-15, -15, MINS_Z};
const vec3_t kMaxs = {15, 15, 32};
constexpr float kGravity = 800;
constexpr float kLeash = 420;  // how far a dummy wanders from its spawn

int RandomMs(int lo, int hi) { return lo + rand() % (hi - lo + 1); }

arena::DummyPattern ParsePattern(const EntityKeys& keys) {
    auto it = keys.find("pattern");
    const std::string p = it == keys.end() ? "strafe" : it->second;
    if (p == "stand") {
        return arena::DummyPattern::kStand;
    }
    if (p == "strafejump") {
        return arena::DummyPattern::kStrafeJump;
    }
    if (p == "circle") {
        return arena::DummyPattern::kCircle;
    }
    return arena::DummyPattern::kStrafe;
}

void Place(gentity_t* e, EntState& st) {
    VectorCopy(st.home, e->r.currentOrigin);
    VectorCopy(st.home, e->s.pos.trBase);
    VectorClear(st.velocity);
    st.health = arena::kMaxHealth;
    st.armor = 0;
    st.deadUntil = 0;
    st.nextTurn = level.time + RandomMs(400, 1200);
    st.nextJump = level.time + RandomMs(300, 800);
    st.heading = st.homeYaw;
    e->s.eFlags &= ~EF_NODRAW;
    e->r.contents = CONTENTS_BODY;
    e->s.time2 = st.health;
    trap::LinkEntity(e);
}

// Move by `delta` with the box, stop at what's hit. Returns the trace.
trace_t MoveBy(gentity_t* e, const vec3_t delta) {
    vec3_t end;
    VectorAdd(e->r.currentOrigin, delta, end);
    trace_t tr;
    trap::Trace(&tr, e->r.currentOrigin, kMins, kMaxs, end, e->s.number, MASK_PLAYERSOLID);
    if (!tr.startsolid) {
        VectorCopy(tr.endpos, e->r.currentOrigin);
    }
    return tr;
}

}  // namespace

bool SpawnDummy(const EntityKeys& keys) {
    vec3_t origin;
    if (!KeyVector(keys, "origin", origin)) {
        Com_Printf(S_COLOR_YELLOW "WARNING: info_dummy without an origin\n");
        return true;
    }
    gentity_t* e = Spawn();
    e->classname = "dummy";
    e->s.eType = ET_GENERAL;
    e->s.generic1 = arena::kKindDummy;
    EntState& st = StateOf(e);
    st = EntState{};
    st.kind = EntState::Kind::kDummy;
    st.pattern = ParsePattern(keys);
    VectorCopy(origin, st.home);
    st.home[2] += 9;  // like player spawns, start just above the marker
    auto it = keys.find("angle");
    st.homeYaw = it == keys.end() ? 0 : static_cast<float>(std::atof(it->second.c_str()));
    e->s.modelindex = static_cast<int>(st.pattern);
    e->s.pos.trType = TR_INTERPOLATE;
    e->s.apos.trType = TR_INTERPOLATE;
    e->s.apos.trBase[YAW] = st.homeYaw;
    VectorCopy(kMins, e->r.mins);
    VectorCopy(kMaxs, e->r.maxs);
    Place(e, st);
    return true;
}

void RunDummy(gentity_t* e) {
    EntState& st = StateOf(e);
    if (st.deadUntil) {
        if (level.time >= st.deadUntil) {
            Place(e, st);
        }
        return;
    }
    const float dt = std::clamp((level.time - level.previousTime) / 1000.0f, 0.0f, 0.05f);

    // wanted horizontal velocity for the pattern
    vec3_t forward, right, angles = {0, st.homeYaw, 0};
    AngleVectors(angles, forward, right, nullptr);
    vec3_t wish = {0, 0, 0};
    switch (st.pattern) {
        case arena::DummyPattern::kStand:
            break;
        case arena::DummyPattern::kStrafe:
        case arena::DummyPattern::kStrafeJump: {
            if (level.time >= st.nextTurn) {
                st.side = -st.side;
                st.nextTurn = level.time + RandomMs(350, 1300);
            }
            const float speed = st.pattern == arena::DummyPattern::kStrafeJump ? 380.0f : 320.0f;
            VectorScale(right, st.side * speed, wish);
            if (st.pattern == arena::DummyPattern::kStrafeJump && st.onGround && level.time >= st.nextJump) {
                st.velocity[2] = 270;
                st.onGround = false;
                st.nextJump = level.time + RandomMs(450, 900);
            }
            break;
        }
        case arena::DummyPattern::kCircle: {
            st.heading += 80.0f * dt;
            const float rad = DEG2RAD(st.heading);
            wish[0] = std::cos(rad) * 300;
            wish[1] = std::sin(rad) * 300;
            break;
        }
    }
    // stay near home: past the leash, head back
    vec3_t toHome;
    VectorSubtract(st.home, e->r.currentOrigin, toHome);
    toHome[2] = 0;
    if (VectorLength(toHome) > kLeash) {
        VectorNormalize(toHome);
        VectorScale(toHome, 320, wish);
        st.side = -st.side;
    }

    // on the ground the wish applies at once; in the air keep momentum (knockback flies)
    if (st.onGround) {
        st.velocity[0] = wish[0];
        st.velocity[1] = wish[1];
    }
    st.velocity[2] -= kGravity * dt;

    // horizontal, then vertical
    vec3_t h = {st.velocity[0] * dt, st.velocity[1] * dt, 0};
    trace_t tr = MoveBy(e, h);
    if (tr.fraction < 1.0f) {
        st.side = -st.side;  // bounced off a wall
        st.heading += 90;
        st.velocity[0] = st.velocity[1] = 0;
    }
    vec3_t v = {0, 0, st.velocity[2] * dt};
    tr = MoveBy(e, v);
    if (tr.fraction < 1.0f) {
        if (st.velocity[2] < 0) {
            st.onGround = true;
        }
        st.velocity[2] = 0;
    } else {
        vec3_t down = {0, 0, -2};
        vec3_t end;
        VectorAdd(e->r.currentOrigin, down, end);
        trace_t g;
        trap::Trace(&g, e->r.currentOrigin, kMins, kMaxs, end, e->s.number, MASK_PLAYERSOLID);
        st.onGround = g.fraction < 1.0f && st.velocity[2] <= 0;
        if (st.onGround) {
            st.velocity[2] = 0;
        }
    }

    VectorCopy(e->r.currentOrigin, e->s.pos.trBase);
    e->s.apos.trBase[YAW] = st.pattern == arena::DummyPattern::kCircle ? st.heading : st.homeYaw;
    e->s.time2 = st.health;
    trap::LinkEntity(e);
}

void DamageDummy(gentity_t* e, gentity_t* attacker, const vec3_t dir, int damage, int /*dflags*/,
                 float knockbackScale) {
    EntState& st = StateOf(e);
    if (attacker && attacker->client) {
        attacker->client->ps.persistant[PERS_HITS]++;
        clients[attacker->s.number].damageDealt += damage;
    }
    // knockback like a player's (mass 200), so rockets juggle them
    if (dir) {
        vec3_t d;
        VectorCopy(dir, d);
        VectorNormalize(d);
        const float knock = static_cast<float>(std::min(damage, 200));
        VectorMA(st.velocity, mcv.knockback.value * knockbackScale * knock / 200.0f, d, st.velocity);
        if (st.velocity[2] > 0) {
            st.onGround = false;
        }
    }
    st.health -= damage;
    e->s.time2 = std::max(0, st.health);
    AddEvent(e, EV_PAIN, e->s.time2);
    if (st.health > 0) {
        return;
    }
    if (attacker && attacker->client) {
        clients[attacker->s.number].kills++;
    }
    gentity_t* tent = TempEvent(e->r.currentOrigin, EV_GIB_PLAYER, 0);
    tent->s.otherEntityNum = e->s.number;
    st.deadUntil = level.time + arena::kRespawnMs;
    e->s.eFlags |= EF_NODRAW;
    e->r.contents = 0;
    trap::LinkEntity(e);
}

}  // namespace sg::mode
