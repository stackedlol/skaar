// This file is part of Skaar. GPLv2 or later.
#include "weapons.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "json.hpp"

namespace skaar {
namespace {

using json = nlohmann::json;

// Q3's values (code/game/g_weapon.c, bg_pmove.c PM_Weapon; GPL), slots as WP_* in bg_public.h
std::vector<WeaponDef> BuiltIns() {
    WeaponDef shotgun;
    shotgun.id = "shotgun";
    shotgun.name = "Shotgun";
    shotgun.slot = 3;
    shotgun.type = FireType::kPellets;
    shotgun.damage = 10;
    shotgun.pellets = 11;
    shotgun.spread = 700;
    shotgun.refireMs = 1000;
    shotgun.ammoStart = 10;
    shotgun.ammoPickup = 10;

    WeaponDef rocket;
    rocket.id = "rocket";
    rocket.name = "Rocket";
    rocket.slot = 5;
    rocket.type = FireType::kProjectile;
    rocket.damage = 100;
    rocket.splashDamage = 100;
    rocket.splashRadius = 120;
    rocket.speed = 900;
    rocket.refireMs = 800;
    rocket.ammoStart = 10;
    rocket.ammoPickup = 5;

    WeaponDef lightning;
    lightning.id = "lightning";
    lightning.name = "Lightning";
    lightning.slot = 6;
    lightning.type = FireType::kBeam;
    lightning.damage = 8;
    lightning.range = 768;
    lightning.refireMs = 50;
    lightning.ammoStart = 100;
    lightning.ammoPickup = 60;

    WeaponDef rail;
    rail.id = "rail";
    rail.name = "Rail";
    rail.slot = 7;
    rail.type = FireType::kHitscan;
    rail.damage = 100;
    rail.pierce = 4;
    rail.refireMs = 1500;
    rail.ammoStart = 10;
    rail.ammoPickup = 10;
    return {shotgun, rocket, lightning, rail};
}

std::vector<WeaponDef>& Registry() {
    static std::vector<WeaponDef> weapons = BuiltIns();
    return weapons;
}

// A JSON number as a float. Checked while it is still a double: narrowing an
// out-of-float-range value is UB, and the value would only be rejected later.
float Float(const json& node, const char* what) {
    const double d = node.get<double>();
    if (!std::isfinite(d) || std::fabs(d) > static_cast<double>(std::numeric_limits<float>::max())) {
        throw std::runtime_error(std::string(what) + " is out of range");
    }
    return static_cast<float>(d);
}

template <typename T>
T Get(const json& j, const char* key, T fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return fallback;
    }
    if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(Float(*it, key));
    } else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        if (!it->is_number()) {
            throw std::runtime_error(std::string(key) + " must be a number");
        }
        // get<int>() narrows with no range test: UB for an out-of-range number and
        // a silent truncation for a fractional one. A float is still a whole number
        // when it is exactly integral -- a hand-written file spells 4 as 4.0 and
        // nlohmann used to truncate it -- so only a genuine fraction is rejected.
        const double d = it->get<double>();
        if (!std::isfinite(d) || std::trunc(d) != d) {
            throw std::runtime_error(std::string(key) + " must be a whole number");
        }
        if (d < static_cast<double>(std::numeric_limits<T>::min()) ||
            d > static_cast<double>(std::numeric_limits<T>::max())) {
            throw std::runtime_error(std::string(key) + " is out of range");
        }
        return static_cast<T>(d);
    } else {
        return it->get<T>();
    }
}

}  // namespace

bool ParseWeapon(std::string_view id, std::string_view text, WeaponDef& out, std::string& error) {
    try {
        const json j = json::parse(text.begin(), text.end(), nullptr, true, true);
        if (!j.is_object()) {
            throw std::runtime_error("a weapon file must be a JSON object");
        }
        // start from the built-in with this id, so a file only needs what it changes
        WeaponDef w;
        for (const WeaponDef& b : BuiltIns()) {
            if (b.id == id) {
                w = b;
            }
        }
        w.id = id;
        w.name = Get<std::string>(j, "name", w.name.empty() ? w.id : w.name);
        w.slot = Get<int>(j, "slot", w.slot);
        if (j.contains("type")) {
            const std::string t = j.at("type").get<std::string>();
            if (t == "hitscan") {
                w.type = FireType::kHitscan;
            } else if (t == "pellets") {
                w.type = FireType::kPellets;
            } else if (t == "beam") {
                w.type = FireType::kBeam;
            } else if (t == "projectile") {
                w.type = FireType::kProjectile;
            } else {
                throw std::runtime_error("type must be hitscan, pellets, beam or projectile");
            }
        }
        w.damage = Get<float>(j, "damage", w.damage);
        w.splashDamage = Get<float>(j, "splashDamage", w.splashDamage);
        w.splashRadius = Get<float>(j, "splashRadius", w.splashRadius);
        w.range = Get<float>(j, "range", w.range);
        w.speed = Get<float>(j, "speed", w.speed);
        w.pellets = Get<int>(j, "pellets", w.pellets);
        w.spread = Get<float>(j, "spread", w.spread);
        w.pierce = Get<int>(j, "pierce", w.pierce);
        w.refireMs = Get<int>(j, "refireMs", w.refireMs);
        w.knockback = Get<float>(j, "knockback", w.knockback);
        w.semiAuto = Get<bool>(j, "semiAuto", w.semiAuto);
        w.tracer = Get<bool>(j, "tracer", w.tracer);
        w.firstShotSpread = Get<float>(j, "firstShotSpread", w.firstShotSpread);
        w.inaccuracy = Get<float>(j, "inaccuracy", w.inaccuracy);
        w.moveInaccuracy = Get<float>(j, "moveInaccuracy", w.moveInaccuracy);
        w.moveSpeedRef = Get<float>(j, "moveSpeedRef", w.moveSpeedRef);
        w.airInaccuracy = Get<float>(j, "airInaccuracy", w.airInaccuracy);
        w.crouchScale = Get<float>(j, "crouchScale", w.crouchScale);
        w.headMultiplier = Get<float>(j, "headMultiplier", w.headMultiplier);
        if (j.contains("falloff")) {
            const json& f = j.at("falloff");
            if (!f.is_array() || f.size() != 3) {
                throw std::runtime_error("falloff must be [start, end, minScale] with start < end, 0 <= minScale <= 1");
            }
            const float start = Float(f[0], "falloff"), end = Float(f[1], "falloff");
            const float minScale = Float(f[2], "falloff");
            if (start < 0 || end <= start || minScale < 0 || minScale > 1) {
                throw std::runtime_error("falloff must be [start, end, minScale] with start < end, 0 <= minScale <= 1");
            }
            w.falloffStart = start;
            w.falloffEnd = end;
            w.falloffMin = minScale;
        }
        w.magazine = Get<int>(j, "magazine", w.magazine);
        w.reloadMs = Get<int>(j, "reloadMs", w.reloadMs);
        if (j.contains("recoil")) {
            const json& r = j.at("recoil");
            w.recoil.clear();
            for (const json& kick : r.at("pattern")) {
                w.recoil.push_back({Float(kick.at(0), "recoil.pattern"), Float(kick.at(1), "recoil.pattern")});
            }
            w.recoilRecoverMs = Get<int>(r, "recoverMs", w.recoilRecoverMs);
            w.cameraFollow = Get<float>(r, "cameraFollow", w.cameraFollow);
            // the modules keep the pause in 15 bits, so a longer one can never reset
            // the spray, and a negative one resets it on every shot
            if (w.recoil.size() > 256 || w.cameraFollow < 0 || w.cameraFollow > 1 || w.recoilRecoverMs < 0 ||
                w.recoilRecoverMs > 32767) {
                throw std::runtime_error("recoil: at most 256 kicks, cameraFollow 0..1, recoverMs 0..32767");
            }
        }
        if (w.magazine < 0 || w.magazine > 1000 || w.reloadMs < 0 || w.reloadMs > 10000) {
            throw std::runtime_error("magazine must be 0..1000 and reloadMs 0..10000");
        }
        // bounded both ways: a huge cone reaches the view-angle maths and turns it NaN
        auto bad = [](float v, float lo, float hi) { return v < lo || v > hi; };
        if (bad(w.inaccuracy, 0, 180) || bad(w.firstShotSpread, 0, 180) || bad(w.moveInaccuracy, 0, 180) ||
            bad(w.airInaccuracy, 0, 180) || bad(w.moveSpeedRef, 1, 2000) || bad(w.crouchScale, 0, 1) ||
            bad(w.spread, 0, 20000) || bad(w.knockback, 0, 100)) {
            throw std::runtime_error("bad inaccuracy / moveSpeedRef / crouchScale values");
        }
        if (bad(w.headMultiplier, 1, 100)) {
            throw std::runtime_error("headMultiplier must be 1..100");
        }
        if (j.contains("ammo")) {
            const json& a = j.at("ammo");
            w.ammoStart = Get<int>(a, "start", w.ammoStart);
            w.ammoMax = Get<int>(a, "max", w.ammoMax);
            w.ammoPickup = Get<int>(a, "pickup", w.ammoPickup);
        }

        if (w.slot < 1 || w.slot > 15) {
            throw std::runtime_error("slot must be 1..15 (a WP_* number)");
        }
        // the damage path does static_cast<int>(damage * headMultiplier), so the product
        // has to stay inside int as well as float
        if (bad(w.damage, 0, 10000) || bad(w.splashDamage, 0, 10000) || bad(w.splashRadius, 0, 10000)) {
            throw std::runtime_error("damage values must be 0..10000");
        }
        if (w.refireMs < 10 || w.refireMs > 10000) {
            throw std::runtime_error("refireMs must be 10..10000");
        }
        if (w.type == FireType::kProjectile && (w.speed < 50 || w.speed > 20000)) {
            throw std::runtime_error("a projectile needs speed 50..20000");
        }
        if (w.pellets < 1 || w.pellets > 64 || w.pierce < 1 || w.pierce > 16) {
            throw std::runtime_error("pellets must be 1..64 and pierce 1..16");
        }
        if (w.range < 16 || w.range > 65536) {
            throw std::runtime_error("range must be 16..65536");
        }
        if (w.ammoStart < 0 || w.ammoMax < 1 || w.ammoPickup < 0) {
            throw std::runtime_error("bad ammo values");
        }
        out = std::move(w);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

void SprayOffset(const WeaponDef& w, int index, float& up, float& right) {
    up = right = 0;
    const int n = std::min(index, static_cast<int>(w.recoil.size()));
    for (int i = 0; i < n; i++) {
        up += w.recoil[i][0];
        right += w.recoil[i][1];
    }
}

float Inaccuracy(const WeaponDef& w, bool firstShot, float horizontalSpeed, bool onGround, bool crouched) {
    float cone = firstShot ? w.firstShotSpread : w.inaccuracy;
    cone += w.moveInaccuracy * std::min(horizontalSpeed / w.moveSpeedRef, 1.5f);
    if (!onGround) {
        cone += w.airInaccuracy;
    }
    if (crouched) {
        cone *= w.crouchScale;
    }
    return cone;
}

void SpreadSample(float coneDeg, int seed, float& up, float& right) {
    // two draws from a small LCG; sqrt keeps the disc uniform
    uint32_t x = static_cast<uint32_t>(seed) * 2654435761u + 0x9e3779b9u;
    auto next = [&x] {
        x = x * 1664525u + 1013904223u;
        return static_cast<float>(x >> 8) / 16777216.0f;
    };
    const float r = coneDeg * std::sqrt(next());
    const float a = next() * 6.2831853f;
    up = r * std::sin(a);
    right = r * std::cos(a);
}

float FalloffScale(const WeaponDef& w, float distance) {
    if (w.falloffEnd <= 0 || distance <= w.falloffStart) {
        return 1;
    }
    if (distance >= w.falloffEnd) {
        return w.falloffMin;
    }
    const float t = (distance - w.falloffStart) / (w.falloffEnd - w.falloffStart);
    return 1 + (w.falloffMin - 1) * t;
}

void ResetWeapons() { Registry() = BuiltIns(); }

void AddWeapon(const WeaponDef& weapon) {
    auto& r = Registry();
    r.erase(std::remove_if(r.begin(), r.end(),
                           [&](const WeaponDef& w) { return w.id == weapon.id || w.slot == weapon.slot; }),
            r.end());
    r.push_back(weapon);
    std::sort(r.begin(), r.end(), [](const WeaponDef& a, const WeaponDef& b) { return a.slot < b.slot; });
}

const WeaponDef* WeaponBySlot(int slot) {
    for (const WeaponDef& w : Registry()) {
        if (w.slot == slot) {
            return &w;
        }
    }
    return nullptr;
}

const WeaponDef* WeaponById(std::string_view id) {
    for (const WeaponDef& w : Registry()) {
        if (w.id == id) {
            return &w;
        }
    }
    return nullptr;
}

const std::vector<WeaponDef>& Weapons() { return Registry(); }

}  // namespace skaar
