// This file is part of Skaar. GPLv2 or later.
#include <algorithm>
#include <cmath>
#include <set>

#include "check.h"
#include "shared/trainer_sim.h"

using namespace skaar;

namespace {

constexpr float kDeg = 57.29578f;
const Vec3 kEye{0, 0, 75};

Scenario Make(const char* json) {
    Scenario s;
    std::string error;
    if (!ParseScenario("test", json, s, error)) {
        check::Fail(__FILE__, __LINE__, "scenario: " + error);
    }
    return s;
}

// yaw/pitch from the eye to a point
void AimAt(Vec3 p, float& yaw, float& pitch) {
    yaw = std::atan2(p.y - kEye.y, p.x - kEye.x) * kDeg;
    pitch = std::atan2(p.z - kEye.z, std::hypot(p.x - kEye.x, p.y - kEye.y)) * kDeg;
}

// A bot that snaps to the oldest alive target and clicks (or holds) each command.
RunRecord Play(const Scenario& sc, uint32_t seed, float hz) {
    TrainerSim sim;
    sim.Start(sc, seed, kEye, 0);
    const float dt = 1 / hz;
    bool was = false;
    for (int i = 0; !sim.Finished(); i++) {
        const float t = i * dt;
        sim.Advance(t);
        const Target* pick = nullptr;
        for (const Target& tg : sim.Targets()) {
            if (tg.alive && (!pick || tg.serial < pick->serial)) {
                pick = &tg;
            }
        }
        float yaw = 0, pitch = 0;
        if (pick) {
            AimAt(sim.TargetPosition(*pick), yaw, pitch);
        }
        const bool down = sc.fire == FireMode::kHold ? true : (i % 20) < 10;  // click every 20 commands
        sim.Input(t, kEye, yaw, pitch, down && !was, down, dt);
        was = down;
    }
    return sim.Record();
}

const char* kGrid = R"({"duration": 20, "targets": {"alive": 3, "radius": 20,
    "spawn": {"shape": "grid", "cols": 4, "rows": 3, "distance": 700, "yaw": [-20, 20], "pitch": [-5, 10]}}})";
const char* kStrafe = R"({"duration": 30, "fire": "hold", "targets": {"alive": 2, "radius": 20, "health": 0.4,
    "spawn": {"distance": [600, 800], "yaw": [-30, 30], "pitch": [0, 12]},
    "motion": {"type": "float", "speed": [150, 400], "changeEvery": [0.2, 0.9]}}})";

}  // namespace

TEST("same seed and inputs give the same run") {
    for (const char* json : {kGrid, kStrafe}) {
        const Scenario sc = Make(json);
        const RunRecord a = Play(sc, 1234, 250), b = Play(sc, 1234, 250);
        CHECK(RecordToJson(a) == RecordToJson(b));
        CHECK(a.summary.kills > 0);
        const RunRecord c = Play(sc, 999, 250);
        CHECK(RecordToJson(a) != RecordToJson(c));  // and the seed matters
    }
}

TEST("target motion doesn't depend on the update rate") {
    const Scenario sc = Make(kStrafe);
    TrainerSim fine, coarse;
    fine.Start(sc, 7, kEye, 0);
    coarse.Start(sc, 7, kEye, 0);
    for (int i = 0; i <= 20000; i++) {
        fine.Advance(i * 0.001f);
    }
    for (float t = 0; t < 20; t += 0.0173f) {
        coarse.Advance(t);
    }
    coarse.Advance(20);
    for (size_t i = 0; i < fine.Targets().size(); i++) {
        CHECK(fine.Targets()[i].serial == coarse.Targets()[i].serial);
        CHECK_NEAR(fine.Targets()[i].yaw, coarse.Targets()[i].yaw, 0.05);
        CHECK_NEAR(fine.Targets()[i].pitch, coarse.Targets()[i].pitch, 0.05);
    }
}

TEST("moving targets stay inside the spawn window") {
    const Scenario sc = Make(kStrafe);
    TrainerSim sim;
    sim.Start(sc, 42, kEye, 0);
    for (int i = 0; i < 30 * 125; i++) {
        sim.Advance(i / 125.0f);
        for (const Target& t : sim.Targets()) {
            if (t.alive && (t.yaw < -30.001f || t.yaw > 30.001f || t.pitch < -0.001f || t.pitch > 12.001f)) {
                check::Fail(__FILE__, __LINE__, "target left the window");
                return;
            }
        }
    }
}

TEST("click: centre hits and kills, empty space misses") {
    const Scenario sc = Make(kGrid);
    TrainerSim sim;
    sim.Start(sc, 5, kEye, 0);
    const Target first = sim.Targets()[0];
    float yaw, pitch;
    AimAt(sim.TargetPosition(first), yaw, pitch);
    TrainerEvents ev = sim.Input(0.5f, kEye, yaw, pitch, true, true, 0.008f);
    CHECK(ev.hits == 1 && ev.kills == 1);
    CHECK(sim.Kills() == 1 && sim.Shots() == 1);
    // the slot respawned with a new serial
    CHECK(sim.Targets()[0].alive && sim.Targets()[0].serial == 3);
    ev = sim.Input(0.6f, kEye, 120, 60, true, true, 0.008f);  // straight up and behind
    CHECK(ev.misses == 1 && ev.hits == 0);
    CHECK_NEAR(sim.Accuracy(), 0.5, 1e-6);
    // docs/scenarios.md: score = kills * kill * ((1 - w) + w * accuracy), with
    // the defaults kill 100 / accuracyWeight 0.5 and 1 kill at accuracy 0.5
    CHECK_NEAR(sim.Score(), 100 * (0.5 + 0.5 * 0.5), 1e-3);
    // held without a new press: no shot
    ev = sim.Input(0.7f, kEye, yaw, pitch, false, true, 0.008f);
    CHECK(sim.Shots() == 2);
    const RunRecord r = sim.Record();
    CHECK(r.shots.size() == 2 && r.shots[0].hit && !r.shots[1].hit);
    CHECK(r.kills.size() == 1 && r.kills[0].target == first.serial);
    CHECK_NEAR(r.kills[0].ttk, 500, 0.5);
    CHECK(std::fabs(r.shots[0].dx) < 0.01f && std::fabs(r.shots[0].dy) < 0.01f);  // dead centre
}

TEST("grid never stacks two targets in one cell") {
    const Scenario sc = Make(kGrid);
    TrainerSim sim;
    sim.Start(sc, 11, kEye, 0);
    for (int k = 0; k < 200; k++) {
        std::set<int> cells;
        for (const Target& t : sim.Targets()) {
            CHECK(t.alive && cells.insert(t.cell).second);
        }
        const Target& victim = sim.Targets()[k % 3];
        float yaw, pitch;
        AimAt(sim.TargetPosition(victim), yaw, pitch);
        sim.Input(0.01f * (k + 1), kEye, yaw, pitch, true, true, 0.01f);
    }
    CHECK(sim.Kills() == 200);
}

TEST("hold: time on target scores and kills") {
    const Scenario sc = Make(R"({"duration": 10, "fire": "hold", "targets": {"alive": 1, "radius": 20, "health": 0.5,
        "spawn": {"distance": 700, "yaw": 0, "pitch": 0}}, "score": {"kill": 100, "perSecondOnTarget": 100}})");
    TrainerSim sim;
    sim.Start(sc, 3, kEye, 0);
    float yaw, pitch;
    AimAt(sim.TargetPosition(sim.Targets()[0]), yaw, pitch);
    // 137 commands at 125 Hz = 1.096 s on target; each target needs 0.5 s (63 commands)
    // and respawns on the same spot (the window is a single point)
    for (int i = 1; i <= 137; i++) {
        sim.Input(i * 0.008f, kEye, yaw, pitch, i == 1, true, 0.008f);
    }
    CHECK_NEAR(sim.Accuracy(), 1.0, 1e-6);
    CHECK(sim.Kills() == 2);
    CHECK_NEAR(sim.Score(), 1.096 * 100 + 2 * 100, 0.5);
    // firing at nothing for as long halves accuracy
    for (int i = 138; i <= 274; i++) {
        sim.Input(i * 0.008f, kEye, 90, 0, false, true, 0.008f);
    }
    CHECK_NEAR(sim.Accuracy(), 0.5, 0.01);
    // and the score still pays for seconds ON TARGET, not for seconds firing
    CHECK_NEAR(sim.Score(), 1.096 * 100 + 2 * 100, 0.5);
}

TEST("timed targets expire on schedule") {
    const Scenario sc = Make(R"({"duration": 10, "targets": {"alive": 2, "radius": 16, "lifetime": 1.0,
        "spawn": {"distance": [650, 900], "yaw": [-35, 35], "pitch": [-6, 20]}}})");
    TrainerSim sim;
    sim.Start(sc, 8, kEye, 0);
    sim.Advance(10);
    // both slots time out every second; the last ones spawn at t=10 exactly
    CHECK(sim.Expired() == 20);
    CHECK(sim.Finished());
}

TEST("replaying recorded kills reproduces the run") {
    const Scenario sc = Make(kGrid);
    const RunRecord original = Play(sc, 77, 125);
    TrainerSim replay;
    replay.Start(sc, original.seed, kEye, 0);
    for (const KillEvent& k : original.kills) {
        CHECK(replay.KillSerial(k.target, k.t));
    }
    replay.Advance(sc.duration);
    CHECK(replay.Kills() == original.summary.kills);
    // the targets alive at the end are the same ones
    TrainerSim again;
    again.Start(sc, original.seed, kEye, 0);
    for (const KillEvent& k : original.kills) {
        again.KillSerial(k.target, k.t);
    }
    again.Advance(sc.duration);
    for (size_t i = 0; i < again.Targets().size(); i++) {
        CHECK(again.Targets()[i].serial == replay.Targets()[i].serial);
    }
}

TEST("angle helpers") {
    CHECK_NEAR(AngleDelta(10, 350), 20, 1e-4);
    CHECK_NEAR(AngleDelta(350, 10), -20, 1e-4);
    CHECK_NEAR(AngleDelta(180, 0), 180, 1e-4);
    const Vec3 d = AnglesToDir(90, 0);
    CHECK_NEAR(d.y, 1, 1e-5);
    CHECK_NEAR(AnglesToDir(0, 90).z, 1, 1e-5);
}

TEST("a real gun in the trainer: refire, spray climbs, a pause resets it") {
    WeaponDef rifle;
    std::string error;
    CHECK(ParseWeapon("rifle", R"({"slot": 2, "type": "hitscan", "refireMs": 100, "firstShotSpread": 0,
        "inaccuracy": 0, "recoil": {"pattern": [[1, 0], [1, 0], [1, 0], [1, 0]], "recoverMs": 400, "cameraFollow": 0.5}})",
                      rifle, error));
    const Scenario sc = Make(R"({"duration": 20, "fire": "hold", "weapon": "rifle", "targets": {"alive": 1, "radius": 10,
        "health": 1000, "spawn": {"distance": 1000, "yaw": 0, "pitch": 0}}})");
    TrainerSim sim;
    sim.Start(sc, 1, kEye, 0, &rifle);
    CHECK(sim.HasWeapon());
    float yaw, pitch;
    AimAt(sim.TargetPosition(sim.Targets()[0]), yaw, pitch);
    // hold for 0.5 s at 125 Hz: one shot per 100 ms = 5 shots; the first hits, the climb misses later ones
    for (int i = 1; i <= 63; i++) {
        sim.Input(i * 0.008f, kEye, yaw, pitch, i == 1, true, 0.008f);
    }
    CHECK(sim.Shots() == 5);
    CHECK(sim.Hits() >= 1 && sim.Hits() < 5);  // 10 u at 1000 u is ~0.57 deg: 1 deg kicks leave it
    float up, right;
    sim.Punch(0.504f + 0.01f, up, right);
    CHECK(up > 0.5f);  // the view kicks up with the spray
    // after a pause longer than recoverMs, the spray starts over and the first shot hits again
    const int hitsBefore = sim.Hits();
    sim.Input(1.5f, kEye, yaw, pitch, true, true, 0.008f);
    CHECK(sim.Hits() == hitsBefore + 1);
    sim.Punch(3.0f, up, right);
    CHECK(up == 0 && right == 0);
}

TEST("moving, airborne and crouching widen the cone a shot goes into") {
    WeaponDef gun;
    std::string error;
    CHECK(ParseWeapon("gun", R"({"slot": 2, "type": "hitscan", "refireMs": 10, "firstShotSpread": 0,
        "inaccuracy": 0, "moveInaccuracy": 2, "moveSpeedRef": 250, "airInaccuracy": 8, "crouchScale": 0.5})",
                      gun, error));
    // one target 1000 u away with a 30 u radius subtends 1.7 deg, so a 1 deg cone
    // still hits every shot while a 2 deg and a 10 deg cone miss more and more
    const Scenario sc = Make(R"({"duration": 20, "fire": "hold", "weapon": "gun", "targets": {"alive": 1, "radius": 30,
        "health": 10000, "spawn": {"distance": 1000, "yaw": 0, "pitch": 0}}})");
    constexpr int kSteps = 100;   // 12 ms commands, so one shot of the 10 ms refire per step
    auto play = [&sc, &gun](float speed, bool onGround, bool crouched, int& hits, int& shots) {
        TrainerSim sim;
        sim.Start(sc, 1, kEye, 0, &gun);
        float yaw, pitch;
        AimAt(sim.TargetPosition(sim.Targets()[0]), yaw, pitch);
        sim.SetMovement(speed, onGround, crouched);
        for (int i = 1; i <= kSteps; i++) {
            sim.Input(i * 0.012f, kEye, yaw, pitch, i == 1, true, 0.012f);
        }
        hits = sim.Hits();
        shots = sim.Shots();
    };
    int stillHits, stillShots, runHits, runShots, airHits, airShots, crouchHits, crouchShots;
    play(0, true, false, stillHits, stillShots);
    play(250, true, false, runHits, runShots);
    play(250, false, false, airHits, airShots);
    play(250, true, true, crouchHits, crouchShots);
    CHECK(stillShots == kSteps && runShots == kSteps && airShots == kSteps && crouchShots == kSteps);
    CHECK(stillHits == stillShots);     // standing still: dead centre, every shot lands
    CHECK(crouchHits == crouchShots);   // crouchScale halves the 2 deg cone back inside the target
    CHECK(runHits < runShots);          // running widens the cone to 2 deg
    CHECK(airHits < runHits);           // and being off the ground adds another 8 deg
}
