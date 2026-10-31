// Arena client side (P6): weapon effects (rail trails, lightning beam,
// rockets, explosions, shotgun puffs), items and practice dummies, weapon
// selection, hit beeps and the arena HUD. Effects use generated sprites and
// sounds (tools/gen_assets.py, scripts/weapons.shader); no Q3 assets.
//
// This file is part of Skaar. GPLv2 or later.
#include "weapons.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <vector>

#include "../shared/arena.h"
#include "../shared/draw2d.h"
#include "../shared/move_pmove.h"
#include "../shared/weapons.h"

namespace cg::weapons {
namespace {

namespace arena = skaar::arena;
namespace kit = skaar::ui;
namespace theme = skaar::ui::theme;
using kit::Align;
using kit::Color;

struct Rgb {
    float r, g, b;
};

// Effect colours by weapon slot: vivid on purpose, they're light, not UI
Rgb WeaponColor(int slot) {
    switch (slot) {
        case WP_SHOTGUN:
            return {1.0f, 0.88f, 0.55f};
        case WP_ROCKET_LAUNCHER:
            return {1.0f, 0.55f, 0.25f};
        case WP_LIGHTNING:
            return {0.65f, 0.8f, 1.0f};
        case WP_RAILGUN:
            return {0.55f, 0.95f, 1.0f};
        default:
            return {1, 1, 1};
    }
}

Rgb ItemColor(const arena::ItemDef& item) {
    switch (item.type) {
        case arena::ItemType::kWeapon:
        case arena::ItemType::kAmmo:
            return WeaponColor(item.slot);
        case arena::ItemType::kHealth:
            return item.amount >= 100 ? Rgb{0.45f, 0.7f, 1.0f} : Rgb{0.5f, 0.88f, 0.55f};
        case arena::ItemType::kArmor:
            return {1.0f, 0.78f, 0.35f};
    }
    return {1, 1, 1};
}

struct Effect {
    enum class Type { kRail, kGlow, kRing, kTracer };
    Type type;
    vec3_t a, b;     // rail: start and end; others: a is the centre
    int start, duration;
    float size0, size1;  // sprite radius from -> to
    Rgb color;
    float alpha;
};

struct Media {
    qhandle_t sphere = 0, capsule = 0;
    qhandle_t target = 0, dummy = 0, glow = 0, beam = 0, bolt = 0, ring = 0;
    sfxHandle_t rail = 0, rocket = 0, explode = 0, shotgun = 0, lightning = 0, pickup = 0, pain = 0, hit = 0;
} media;

struct State {
    std::vector<Effect> effects;
    int selected = 0;          // slot the player picked (0: follow ps.weapon)
    int lastHits = -1;         // ps.persistant[PERS_HITS] last frame
    int lastHitSound = 0;
    int lightningCount = 0;
    std::array<int, MAX_GENTITIES> painTime{};
    std::array<int, MAX_GENTITIES> lastTrail{};
    std::array<sfxHandle_t, 16> fireSound{};  // sound/weapons/<id>.wav by slot
} st;

void Sound(sfxHandle_t s) {
    if (s) {
        trap::S_StartLocalSound(s, CHAN_LOCAL_SOUND);
    }
}

void AddEffect(Effect e) {
    if (st.effects.size() < 512) {
        st.effects.push_back(e);
    }
}

void Glow(const vec3_t at, float size0, float size1, int duration, Rgb color, float alpha = 1) {
    Effect e{};
    e.type = Effect::Type::kGlow;
    VectorCopy(at, e.a);
    e.start = cg.time;
    e.duration = duration;
    e.size0 = size0;
    e.size1 = size1;
    e.color = color;
    e.alpha = alpha;
    AddEffect(e);
}

void Ring(const vec3_t at, float size0, float size1, int duration, Rgb color) {
    Effect e{};
    e.type = Effect::Type::kRing;
    VectorCopy(at, e.a);
    e.start = cg.time;
    e.duration = duration;
    e.size0 = size0;
    e.size1 = size1;
    e.color = color;
    e.alpha = 1;
    AddEffect(e);
}

void SetColor(refEntity_t& re, Rgb c, float alpha) {
    re.shaderRGBA[0] = static_cast<byte>(std::clamp(c.r, 0.0f, 1.0f) * 255);
    re.shaderRGBA[1] = static_cast<byte>(std::clamp(c.g, 0.0f, 1.0f) * 255);
    re.shaderRGBA[2] = static_cast<byte>(std::clamp(c.b, 0.0f, 1.0f) * 255);
    re.shaderRGBA[3] = static_cast<byte>(std::clamp(alpha, 0.0f, 1.0f) * 255);
}

void AddSprite(const vec3_t at, float radius, qhandle_t shader, Rgb color, float alpha) {
    refEntity_t re{};
    re.reType = RT_SPRITE;
    VectorCopy(at, re.origin);
    re.radius = radius;
    re.customShader = shader;
    re.renderfx = RF_NOSHADOW;
    SetColor(re, color, alpha);
    trap::R_AddRefEntityToScene(&re);
}

void AddModel(qhandle_t model, qhandle_t shader, const vec3_t at, float yaw, float scale, Rgb color) {
    refEntity_t re{};
    re.reType = RT_MODEL;
    re.hModel = model;
    re.customShader = shader;
    re.renderfx = RF_NOSHADOW;
    VectorCopy(at, re.origin);
    VectorCopy(at, re.oldorigin);
    vec3_t angles = {0, yaw, 0};
    AnglesToAxis(angles, re.axis);
    if (scale != 1) {
        for (auto& row : re.axis) {
            VectorScale(row, scale, row);
        }
        re.nonNormalizedAxes = qtrue;
    }
    SetColor(re, color, 1);
    trap::R_AddRefEntityToScene(&re);
}

// World-only trace from the client (impacts, beam ends)
void WorldTrace(trace_t& tr, const vec3_t start, const vec3_t end) {
    trap::CM_BoxTrace(&tr, start, end, nullptr, nullptr, 0, MASK_SOLID);
}

bool Owns(const playerState_t& ps, int slot) { return slot > 0 && slot < 16 && (ps.stats[STAT_WEAPONS] & (1 << slot)); }

void Cycle(int dir) {
    const playerState_t& ps = cg.predicted;
    int slot = SelectedWeapon();
    for (int i = 0; i < 16; i++) {
        slot = (slot + dir + 16) % 16;
        if (Owns(ps, slot) && skaar::WeaponBySlot(slot)) {
            st.selected = slot;
            return;
        }
    }
}

// Shotgun impacts: rebuild Q3's pattern from the event's seed and direction
void ShotgunPuffs(const entityState_t& es) {
    int seed = es.eventParm;
    const float spread = es.angles2[0] > 0 ? es.angles2[0] : 700;
    const int pellets = es.angles2[1] > 0 ? static_cast<int>(es.angles2[1]) : 11;
    vec3_t forward, right, up;
    VectorNormalize2(es.origin2, forward);
    PerpendicularVector(right, forward);
    CrossProduct(forward, right, up);
    for (int i = 0; i < pellets; i++) {
        const float r = Q_crandom(&seed) * spread * 16;
        const float u = Q_crandom(&seed) * spread * 16;
        vec3_t end;
        VectorMA(es.pos.trBase, 8192 * 16, forward, end);
        VectorMA(end, r, right, end);
        VectorMA(end, u, up, end);
        trace_t tr;
        WorldTrace(tr, es.pos.trBase, end);
        if (tr.fraction < 1.0f) {
            Glow(tr.endpos, 3, 9, 260, WeaponColor(WP_SHOTGUN), 0.8f);
        }
    }
}

void DrawEffects() {
    st.effects.erase(std::remove_if(st.effects.begin(), st.effects.end(),
                                    [](const Effect& e) { return cg.time - e.start >= e.duration; }),
                     st.effects.end());
    for (const Effect& e : st.effects) {
        const float t = std::clamp(static_cast<float>(cg.time - e.start) / e.duration, 0.0f, 1.0f);
        const float fade = (1 - t) * e.alpha;
        switch (e.type) {
            case Effect::Type::kRail: {
                refEntity_t re{};
                re.reType = RT_RAIL_CORE;
                re.customShader = media.beam;
                VectorCopy(e.a, re.origin);
                VectorCopy(e.b, re.oldorigin);
                SetColor(re, e.color, fade);
                trap::R_AddRefEntityToScene(&re);
                break;
            }
            case Effect::Type::kTracer: {
                // a short bright streak travelling from the muzzle to the impact
                vec3_t dir, head, tail;
                VectorSubtract(e.b, e.a, dir);
                const float len = VectorNormalize(dir);
                const float travel = std::min(1.0f, t * 3.0f) * len;
                VectorMA(e.a, travel, dir, head);
                VectorMA(e.a, std::max(0.0f, travel - 160.0f), dir, tail);
                refEntity_t re{};
                re.reType = RT_RAIL_CORE;
                re.customShader = media.beam;
                VectorCopy(tail, re.origin);
                VectorCopy(head, re.oldorigin);
                SetColor(re, e.color, 0.55f * fade);
                trap::R_AddRefEntityToScene(&re);
                break;
            }
            case Effect::Type::kGlow:
                AddSprite(e.a, e.size0 + (e.size1 - e.size0) * t, media.glow, e.color, fade);
                break;
            case Effect::Type::kRing:
                AddSprite(e.a, e.size0 + (e.size1 - e.size0) * t, media.ring, e.color, fade);
                break;
        }
    }
}

// The local player's lightning beam: drawn from the predicted view while firing
void DrawLightning() {
    const playerState_t& ps = cg.predicted;
    const skaar::WeaponDef* w = skaar::WeaponBySlot(ps.weapon);
    if (!w || w->type != skaar::FireType::kBeam || !(ps.eFlags & EF_FIRING) || ps.pm_type == PM_DEAD) {
        return;
    }
    vec3_t forward, right, up, start, end;
    AngleVectors(cg.viewAngles, forward, right, up);
    VectorCopy(cg.refdef.vieworg, start);
    VectorMA(start, 14, forward, start);
    VectorMA(start, 6, right, start);   // from just right of and below the eye, like a held gun
    VectorMA(start, -6, up, start);
    vec3_t aimEnd;
    VectorMA(cg.refdef.vieworg, w->range, forward, aimEnd);
    trace_t tr;
    WorldTrace(tr, cg.refdef.vieworg, aimEnd);
    VectorCopy(tr.endpos, end);

    refEntity_t re{};
    re.reType = RT_LIGHTNING;
    re.customShader = media.bolt;
    VectorCopy(start, re.origin);
    VectorCopy(end, re.oldorigin);
    SetColor(re, WeaponColor(WP_LIGHTNING), 1);
    trap::R_AddRefEntityToScene(&re);
    if (tr.fraction < 1.0f) {
        AddSprite(end, 10 + 4 * std::sin(cg.time * 0.05f), media.glow, WeaponColor(WP_LIGHTNING), 0.9f);
    }
}

void HitBeep() {
    const int hits = cg.predicted.persistant[PERS_HITS];
    if (st.lastHits >= 0 && hits > st.lastHits && cg.time - st.lastHitSound > 60) {
        Sound(media.hit);
        st.lastHitSound = cg.time;
    }
    st.lastHits = hits;
}

void LoadWeapons() {
    // the built-ins, overridden by their files (the client only needs names and kinds)
    skaar::ResetWeapons();
    for (const char* id : {"shotgun", "rocket", "lightning", "rail", "rifle", "handcannon"}) {
        std::string text, error;
        skaar::WeaponDef w;
        if (trap::FileRead(skaar::Va("weapons/%s.json", id), text) && skaar::ParseWeapon(id, text, w, error)) {
            skaar::AddWeapon(w);
        }
    }
    // the server applies the same table (sgame mode.cpp), so prediction and firing agree
    skaar::move::ClearWeaponRefire();
    skaar::move::ClearWeaponSemiAuto();
    for (const skaar::WeaponDef& w : skaar::Weapons()) {
        skaar::move::SetWeaponRefire(w.slot, w.refireMs);
        skaar::move::SetWeaponSemiAuto(w.slot, w.semiAuto);
    }
}

}  // namespace

void Init() {
    st = State{};
    LoadWeapons();
    media.sphere = trap::R_RegisterModel("models/skaar/target.md3");
    media.capsule = trap::R_RegisterModel("models/skaar/dummy.md3");
    media.target = trap::R_RegisterShader("skaar/target");
    media.dummy = trap::R_RegisterShader("skaar/dummy");
    media.glow = trap::R_RegisterShader("skaar/fx_glow");
    media.beam = trap::R_RegisterShader("skaar/fx_beam");
    media.bolt = trap::R_RegisterShader("skaar/fx_bolt");
    media.ring = trap::R_RegisterShader("skaar/fx_ring");
    media.rail = trap::S_RegisterSound("sound/weapons/rail.wav", false);
    media.rocket = trap::S_RegisterSound("sound/weapons/rocket.wav", false);
    media.explode = trap::S_RegisterSound("sound/weapons/explode.wav", false);
    media.shotgun = trap::S_RegisterSound("sound/weapons/shotgun.wav", false);
    media.lightning = trap::S_RegisterSound("sound/weapons/lightning.wav", false);
    media.pickup = trap::S_RegisterSound("sound/items/pickup.wav", false);
    media.pain = trap::S_RegisterSound("sound/player/pain.wav", false);
    media.hit = trap::S_RegisterSound("sound/feedback/hit.wav", false);
    for (const skaar::WeaponDef& w : skaar::Weapons()) {
        st.fireSound[w.slot] = trap::S_RegisterSound(skaar::Va("sound/weapons/%s.wav", w.id.c_str()), false);
    }
    for (const char* cmd : {"weapon", "weapnext", "weapprev", "reload"}) {
        trap::AddCommand(cmd);  // reload goes on to the server (mode.cpp)
    }
}

void Shutdown() {
    for (const char* cmd : {"weapon", "weapnext", "weapprev", "reload"}) {
        trap::RemoveCommand(cmd);
    }
}

void AddPacketEntity(const CEntity& cent) {
    const entityState_t& es = cent.current;
    if (es.eFlags & EF_NODRAW) {
        return;
    }
    const float bob = std::sin((cg.time + es.number * 137) * 0.004f);
    if (es.eType == ET_MISSILE) {
        const Rgb c = WeaponColor(es.weapon);
        AddModel(media.sphere, media.target, cent.lerpOrigin, 0, 6.0f / 64.0f, c);
        AddSprite(cent.lerpOrigin, 22, media.glow, c, 0.9f);
        // smoke trail: a fading glow every 25 ms
        if (cg.time - st.lastTrail[es.number] > 25) {
            st.lastTrail[es.number] = cg.time;
            Glow(cent.lerpOrigin, 8, 18, 450, {0.85f, 0.82f, 0.78f}, 0.35f);
        }
        return;
    }
    if (es.eType == ET_ITEM && es.generic1 == arena::kKindItem && es.modelindex >= 0 && es.modelindex < arena::kNumItems) {
        const arena::ItemDef& item = arena::kItems[es.modelindex];
        vec3_t at;
        VectorCopy(cent.lerpOrigin, at);
        at[2] += 4 * bob;
        const float size = item.type == arena::ItemType::kAmmo ? 6.0f : item.amount >= 100 ? 14.0f : 10.0f;
        const Rgb c = ItemColor(item);
        AddModel(media.sphere, media.target, at, 0, size / 64.0f, c);
        AddSprite(at, size * 2.4f, media.glow, c, 0.35f + 0.15f * bob);
        if (item.type == arena::ItemType::kWeapon) {
            // weapons: a slow ring around the core
            AddSprite(at, 22, media.ring, c, 0.6f);
        }
        return;
    }
    if (es.eType == ET_GENERAL && es.generic1 == arena::kKindDummy) {
        Rgb c{theme::kEmber.r, theme::kEmber.g, theme::kEmber.b};
        if (cg.time - st.painTime[es.number] < 90) {
            c = {1, 1, 1};  // hit flash
        }
        AddModel(media.capsule, media.dummy, cent.lerpOrigin, cent.lerpAngles[YAW], 1, c);
        return;
    }
}

void AddLocalEntities() {
    DrawEffects();
    DrawLightning();
}

void EntityEvent(const entityState_t& es, int event) {
    switch (event) {
        case EV_RAILTRAIL: {
            if (const skaar::WeaponDef* w = skaar::WeaponBySlot(es.weapon); w && w->tracer) {
                Effect e{};
                e.type = Effect::Type::kTracer;
                VectorCopy(es.pos.trBase, e.a);
                VectorCopy(es.origin2, e.b);
                e.start = cg.time;
                e.duration = 140;
                e.color = {1.0f, 0.92f, 0.7f};
                e.alpha = 1;
                AddEffect(e);
                Glow(es.origin2, 3, 8, 220, {1.0f, 0.9f, 0.7f}, 0.8f);
                break;
            }
            Effect e{};
            e.type = Effect::Type::kRail;
            VectorCopy(es.pos.trBase, e.a);
            VectorCopy(es.origin2, e.b);
            e.start = cg.time;
            e.duration = 600;
            e.color = WeaponColor(WP_RAILGUN);
            e.alpha = 1;
            AddEffect(e);
            Ring(es.origin2, 6, 20, 350, WeaponColor(WP_RAILGUN));
            if (es.clientNum != cg.clientNum) {
                Sound(media.rail);
            }
            break;
        }
        case EV_SHOTGUN:
            ShotgunPuffs(es);
            break;
        case EV_MISSILE_HIT:
        case EV_MISSILE_MISS: {
            const Rgb c = WeaponColor(es.weapon);
            const skaar::WeaponDef* w = skaar::WeaponBySlot(es.weapon);
            const float radius = w && w->splashRadius > 0 ? w->splashRadius : 60;
            Glow(es.pos.trBase, 16, radius * 0.9f, 420, c);
            Glow(es.pos.trBase, 8, radius * 0.45f, 260, {1, 0.95f, 0.85f});
            Ring(es.pos.trBase, 10, radius, 380, c);
            Sound(media.explode);
            break;
        }
        case EV_GIB_PLAYER: {
            // a dummy (or player) goes down
            const Rgb c{theme::kEmber.r, theme::kEmber.g, theme::kEmber.b};
            Glow(es.pos.trBase, 20, 70, 380, c);
            Ring(es.pos.trBase, 12, 60, 420, c);
            break;
        }
        case EV_PAIN:
            if (es.number >= 0 && es.number < MAX_GENTITIES) {
                st.painTime[es.number] = cg.time;
            }
            break;
        case EV_ITEM_RESPAWN:
            Ring(es.pos.trBase, 8, 34, 500, {1, 1, 1});
            break;
        default:
            break;
    }
}

void PlayerEvent(int event, int /*eventParm*/, const playerState_t& ps) {
    switch (event) {
        case EV_FIRE_WEAPON:
            if (ps.weapon == WP_LIGHTNING && st.lightningCount++ % 2) {
                break;  // its buzz is 120 ms, refire 50
            }
            if (ps.weapon > 0 && ps.weapon < 16) {
                Sound(st.fireSound[ps.weapon]);
            }
            break;
        case EV_ITEM_PICKUP:
            Sound(media.pickup);
            break;
        case EV_PAIN:
            Sound(media.pain);
            break;
        default:
            break;
    }
}

void DrawHud() {
    HitBeep();  // hit feedback stays on with the HUD hidden
    const playerState_t& ps = cg.predicted;
    if (!cvars.drawHud.integer || !ps.stats[STAT_WEAPONS] || (trap::Key_GetCatcher() & KEYCATCH_UI)) {
        return;  // HUD off, no arena loadout (the trainer, free roam), or a menu: nothing to show
    }
    const kit::Fonts& f = kit::GetFonts();
    const float w = kit::Width(), h = kit::Height(), m = theme::kMargin / 2;
    const Color ink = theme::kCream, soft = theme::kCreamSoft, faint = theme::kCreamFaint;

    if (ps.pm_type == PM_DEAD) {
        f.display.Draw(w / 2, h * 0.42f, 64, "DOWN", ink, 0.02f, Align::kCenter);
        f.text.Draw(w / 2, h * 0.42f + 48, 22, "Fire or jump to respawn", soft, 0, Align::kCenter);
        return;
    }

    // health and armor, bottom left
    const int health = ps.stats[STAT_HEALTH], armor = ps.stats[STAT_ARMOR];
    f.display.Draw(m, h - 52, 64, skaar::Va("%d", std::max(0, health)), health <= 25 ? theme::kEmber : ink, -0.01f);
    const float hw = f.display.Measure(64, skaar::Va("%d", std::max(0, health)), -0.01f);
    f.text.Draw(m + hw + 24, h - 52, 30, skaar::Va("%d", armor), armor > 0 ? soft : faint, 0);

    // current weapon and ammo, bottom right
    if (const skaar::WeaponDef* cur = skaar::WeaponBySlot(ps.weapon)) {
        // ps.ammo travels as signed 16-bit (msg.c), so a weapon file with an
        // ammo.max above 32767 arrives negative: the low 16 bits are still the
        // count itself, up to 65535. -1 is sv_infiniteAmmo's marker, not a wrap.
        const int ammo = ps.ammo[ps.weapon] == -1 ? -1 : (ps.ammo[ps.weapon] & 0xffff);
        float right = w - m;
        if (cur->magazine > 0) {
            // magazine weapons: rounds in the magazine, the reserve beside it
            const int reserve = ps.stats[arena::kStatReserve];
            const std::string res = reserve < 0 ? "—" : skaar::Va("%d", reserve);
            f.text.Draw(right, h - 52, 30, res, faint, 0, Align::kRight);
            right -= f.text.Measure(30, res) + 22;
        }
        f.display.Draw(right, h - 52, 64, ammo < 0 ? "—" : skaar::Va("%d", ammo), ammo == 0 ? theme::kEmber : ink,
                       -0.01f, Align::kRight);
        const bool reloading = ps.stats[arena::kStatReloading] != 0;
        f.text.Draw(w - m, h - 100 - 30, 20, reloading ? "RELOADING" : kit::Upper(cur->name), reloading ? ink : soft,
                    0.14f, Align::kRight);
    }

    // owned weapons, bottom centre: the held one bright and underlined
    std::vector<const skaar::WeaponDef*> owned;
    for (const skaar::WeaponDef& wd : skaar::Weapons()) {
        if (Owns(ps, wd.slot)) {
            owned.push_back(&wd);
        }
    }
    float total = 0;
    for (const auto* wd : owned) {
        total += f.text.Measure(18, kit::Upper(wd->name), 0.14f) + 36;
    }
    float x = w / 2 - total / 2;
    for (const auto* wd : owned) {
        const std::string label = kit::Upper(wd->name);
        const float lw = f.text.Measure(18, label, 0.14f);
        const bool held = wd->slot == ps.weapon;
        f.text.Draw(x, h - 56, 18, label, held ? ink : faint, 0.14f);
        f.mono.Draw(x, h - 80, 13, skaar::Va("%d", wd->slot), faint, 0);
        if (held) {
            kit::Fill(x, h - 46, lw, 2, theme::kEmber);
        }
        x += lw + 36;
    }
}

void ViewOffset(vec3_t /*origin*/, vec3_t angles) {
    const playerState_t& ps = cg.predicted;
    const skaar::WeaponDef* w = skaar::WeaponBySlot(ps.weapon);
    if (!w || w->recoil.empty() || ps.pm_type == PM_DEAD || ps.stats[arena::kStatSpray] <= 0) {
        return;
    }
    const int since = (cg.time - ps.stats[arena::kStatLastShot]) & 0x7fff;
    if (w->recoilRecoverMs <= 0 || since >= w->recoilRecoverMs) {
        return;
    }
    // full while spraying, easing back to zero over the recovery window
    const float t = static_cast<float>(since) / w->recoilRecoverMs;
    const float k = w->cameraFollow * (1 - t * t);
    float up, right;
    skaar::SprayOffset(*w, ps.stats[arena::kStatSpray], up, right);
    angles[PITCH] -= up * k;
    angles[YAW] -= right * k;
}

bool HudActive() { return cg.predicted.stats[STAT_WEAPONS] != 0; }

int SelectedWeapon() {
    const playerState_t& ps = cg.predicted;
    if (Owns(ps, st.selected)) {
        return st.selected;
    }
    return Owns(ps, ps.weapon) ? ps.weapon : 0;
}

bool ConsoleCommand(const char* cmd) {
    if (!Q_stricmp(cmd, "weapon")) {
        const int slot = std::atoi(trap::Argv(1).c_str());
        if (Owns(cg.predicted, slot)) {
            st.selected = slot;
        }
        return true;
    }
    if (!Q_stricmp(cmd, "weapnext")) {
        Cycle(1);
        return true;
    }
    if (!Q_stricmp(cmd, "weapprev")) {
        Cycle(-1);
        return true;
    }
    return false;
}

}  // namespace cg::weapons
