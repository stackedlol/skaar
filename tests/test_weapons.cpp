// This file is part of Skaar. GPLv2 or later.
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "check.h"
#include "shared/weapons.h"

using namespace skaar;

namespace {

std::string Slurp(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST("shipped weapons parse and match the built-in defaults") {
    ResetWeapons();
    const auto dir = std::filesystem::path(SKAAR_SOURCE_DIR) / "data" / "base" / "weapons";
    int n = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        WeaponDef w;
        std::string error;
        const std::string id = e.path().stem().string();
        if (!ParseWeapon(id, Slurp(e.path()), w, error)) {
            check::Fail(__FILE__, __LINE__, id + ": " + error);
            continue;
        }
        const WeaponDef* builtin = WeaponById(id);  // files for new weapons (rifle, ...) have no built-in
        if (builtin) {
            CHECK(w.slot == builtin->slot && w.type == builtin->type && w.refireMs == builtin->refireMs);
            CHECK_NEAR(w.damage, builtin->damage, 1e-6);
        }
        n++;
    }
    CHECK(n >= 4);
}

TEST("weapon files only need what they change") {
    WeaponDef w;
    std::string error;
    CHECK(ParseWeapon("rail", R"({"damage": 80})", w, error));
    CHECK(w.slot == 7 && w.pierce == 4 && w.refireMs == 1500);
    CHECK_NEAR(w.damage, 80, 1e-6);
    // a new weapon needs a slot
    CHECK(!ParseWeapon("pistol", R"({"type": "hitscan", "damage": 40})", w, error));
    CHECK(ParseWeapon("pistol", R"({"slot": 2, "type": "hitscan", "damage": 40, "refireMs": 250})", w, error));
    CHECK(!ParseWeapon("bad", R"({"slot": 9, "type": "projectile", "speed": 0})", w, error));
    CHECK(!ParseWeapon("bad", R"({"slot": 9, "type": "laser"})", w, error));
}

TEST("weapon numbers out of range are rejected, not narrowed") {
    WeaponDef w;
    std::string error;
    // a JSON float int range-checks nothing: 1e300 used to reach static_cast<int>
    CHECK(!ParseWeapon("rail", R"({"refireMs": 1e300})", w, error));
    CHECK(!ParseWeapon("rail", R"({"slot": 1e300})", w, error));
    CHECK(!ParseWeapon("rail", R"({"pellets": 1e30})", w, error));
    CHECK(!ParseWeapon("rail", R"({"magazine": -1e30})", w, error));
    // and a fractional value used to truncate silently
    CHECK(!ParseWeapon("rail", R"({"refireMs": 12.5})", w, error));
    CHECK(!ParseWeapon("rail", R"({"slot": 3.5})", w, error));
    CHECK(!ParseWeapon("rail", R"({"magazine": 9.5})", w, error));

    // damage and the cone: both feed a float->int cast and the view angles
    CHECK(!ParseWeapon("rifle", R"({"slot": 2, "damage": 1e30})", w, error));
    CHECK(!ParseWeapon("rifle", R"({"slot": 2, "damage": 1e300})", w, error));
    CHECK(!ParseWeapon("rocket", R"({"splashDamage": 1e30})", w, error));
    CHECK(!ParseWeapon("rocket", R"({"splashRadius": 1e30})", w, error));
    CHECK(!ParseWeapon("rifle", R"({"slot": 2, "headMultiplier": 1e6})", w, error));
    CHECK(!ParseWeapon("rifle", R"({"slot": 2, "crouchScale": 1e300})", w, error));
    CHECK(!ParseWeapon("rifle", R"({"slot": 2, "crouchScale": -4})", w, error));
    CHECK(!ParseWeapon("shotgun", R"({"spread": 1e300})", w, error));
    // shipped-scale values are still accepted
    CHECK(ParseWeapon("rifle", R"({"slot": 2, "damage": 36, "headMultiplier": 4, "crouchScale": 0.6})", w, error));
    CHECK(ParseWeapon("rocket", R"({"splashDamage": 100, "splashRadius": 120})", w, error));
}

TEST("recoil.recoverMs is bounded to what the game modules can carry") {
    WeaponDef w;
    std::string error;
    // the pause is kept in 15 bits: 32768+ can never reset the spray, <0 never leaves it
    CHECK(ParseWeapon("rifle", R"({"slot": 2, "recoil": {"pattern": [[1, 0]], "recoverMs": 0}})", w, error));
    CHECK(ParseWeapon("rifle", R"({"slot": 2, "recoil": {"pattern": [[1, 0]], "recoverMs": 32767}})", w, error));
    CHECK(!ParseWeapon("rifle", R"({"slot": 2, "recoil": {"pattern": [[1, 0]], "recoverMs": 32768}})", w, error));
    CHECK(!ParseWeapon("rifle", R"({"slot": 2, "recoil": {"pattern": [[1, 0]], "recoverMs": -5}})", w, error));
    CHECK(!ParseWeapon("rifle", R"({"slot": 2, "recoil": {"pattern": [[1, 0]], "recoverMs": 1e300}})", w, error));
    CHECK(!ParseWeapon("rifle", R"({"slot": 2, "recoil": {"pattern": [[1e300, 0]]}})", w, error));
}

TEST("a weapon file must be a JSON object") {
    WeaponDef w;
    std::string error;
    // these used to "parse" as the built-in weapon, untouched, with no error
    CHECK(!ParseWeapon("rail", "null", w, error) && !error.empty());
    CHECK(!ParseWeapon("rail", "42", w, error) && !error.empty());
    CHECK(!ParseWeapon("rail", R"("hi")", w, error) && !error.empty());
    CHECK(!ParseWeapon("rail", "[1, 2, 3]", w, error) && !error.empty());
}

TEST("weapon registry by slot and id") {
    ResetWeapons();
    CHECK(WeaponBySlot(7) && WeaponBySlot(7)->id == "rail");
    CHECK(WeaponBySlot(5) && WeaponBySlot(5)->type == FireType::kProjectile);
    WeaponDef pistol;
    std::string error;
    CHECK(ParseWeapon("pistol", R"({"slot": 2, "damage": 40})", pistol, error));
    AddWeapon(pistol);
    CHECK(Weapons().size() == 5 && Weapons().front().id == "pistol");  // sorted by slot
    WeaponDef rail2 = *WeaponById("rail");
    rail2.damage = 70;
    AddWeapon(rail2);
    CHECK(Weapons().size() == 5);
    CHECK_NEAR(WeaponById("rail")->damage, 70, 1e-6);
    ResetWeapons();
}

TEST("tac math: spray, inaccuracy, spread, falloff") {
    WeaponDef w;
    std::string error;
    CHECK(ParseWeapon("rifle", R"({"slot": 2, "type": "hitscan", "damage": 36, "refireMs": 100,
        "firstShotSpread": 0.1, "inaccuracy": 0.25, "moveInaccuracy": 4, "moveSpeedRef": 250, "airInaccuracy": 8,
        "crouchScale": 0.5, "headMultiplier": 4, "falloff": [600, 1600, 0.75], "magazine": 30, "reloadMs": 2500,
        "recoil": {"pattern": [[0.5, 0], [0.5, 0.1], [0.4, -0.2]], "recoverMs": 400, "cameraFollow": 0.5}})", w, error));
    float up, right;
    SprayOffset(w, 0, up, right);
    CHECK_NEAR(up, 0, 1e-6);
    SprayOffset(w, 2, up, right);
    CHECK_NEAR(up, 1.0, 1e-6);
    CHECK_NEAR(right, 0.1, 1e-6);
    SprayOffset(w, 50, up, right);  // past the pattern: holds at the end
    CHECK_NEAR(up, 1.4, 1e-6);
    CHECK_NEAR(right, -0.1, 1e-6);

    CHECK_NEAR(Inaccuracy(w, true, 0, true, false), 0.1, 1e-6);
    CHECK_NEAR(Inaccuracy(w, false, 125, true, false), 0.25 + 2, 1e-5);
    CHECK_NEAR(Inaccuracy(w, false, 0, false, false), 8.25, 1e-5);
    CHECK_NEAR(Inaccuracy(w, false, 0, true, true), 0.125, 1e-6);

    // deterministic, inside the cone, not all the same
    float u1, r1, u2, r2;
    SpreadSample(2, 1234, u1, r1);
    SpreadSample(2, 1234, u2, r2);
    CHECK(u1 == u2 && r1 == r2);
    bool differs = false;
    for (int s = 0; s < 200; s++) {
        SpreadSample(2, s, u2, r2);
        CHECK(std::hypot(u2, r2) <= 2.0001f);
        differs = differs || u2 != u1;
    }
    CHECK(differs);

    CHECK_NEAR(FalloffScale(w, 300), 1, 1e-6);
    CHECK_NEAR(FalloffScale(w, 1100), 0.875, 1e-6);
    CHECK_NEAR(FalloffScale(w, 5000), 0.75, 1e-6);
    CHECK(!ParseWeapon("x", R"({"slot": 2, "falloff": [100, 50, 0.5]})", w, error));
}
