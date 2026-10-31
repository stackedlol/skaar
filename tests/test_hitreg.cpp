// Hit registration (Phase 2, workstream A): edges, input rates, frame spikes.
// The trainer evaluates every user command against the targets at that
// command's time, so results must not depend on the frame rate.
//
// This file is part of Skaar. GPLv2 or later.
#include <cmath>

#include "check.h"
#include "shared/trainer_sim.h"

using namespace skaar;

namespace {

constexpr float kDeg = 57.29578f;
const Vec3 kEye{0, 0, 75};

Scenario Make(const char* json) {
    Scenario s;
    std::string error;
    if (!ParseScenario("hitreg", json, s, error)) {
        check::Fail(__FILE__, __LINE__, "scenario: " + error);
    }
    return s;
}

void AimAt(Vec3 p, float& yaw, float& pitch) {
    yaw = std::atan2(p.y - kEye.y, p.x - kEye.x) * kDeg;
    pitch = std::atan2(p.z - kEye.z, std::hypot(p.x - kEye.x, p.y - kEye.y)) * kDeg;
}

// Perfect tracking of target 0 with commands spaced by dts[i % n]; the run record
RunRecord TrackWithSteps(const Scenario& sc, const std::vector<float>& dts) {
    TrainerSim sim;
    sim.Start(sc, 99, kEye, 0);
    float t = 0;
    for (size_t i = 0; !sim.Finished() && t < sc.duration; i++) {
        const float dt = dts[i % dts.size()];
        t += dt;
        sim.Advance(t);
        float yaw, pitch;
        AimAt(sim.TargetPosition(sim.Targets()[0]), yaw, pitch);
        sim.Input(t, kEye, yaw, pitch, i == 0, true, dt);
    }
    return sim.Record();
}

}  // namespace

TEST("hit at the target's edge, miss just outside") {
    const Scenario sc = Make(R"({"duration": 10, "targets": {"alive": 1, "radius": 20, "health": 100,
        "spawn": {"distance": 1000, "yaw": 0, "pitch": 0}}})");
    TrainerSim sim;
    sim.Start(sc, 1, kEye, 0);
    float yaw, pitch;
    AimAt(sim.TargetPosition(sim.Targets()[0]), yaw, pitch);
    const float dist = 1000;  // eye to centre (same height)
    const float inside = std::asin(19.9f / dist) * kDeg, outside = std::asin(20.1f / dist) * kDeg;
    int hits = 0;
    for (float off : {inside, -inside}) {
        hits += sim.Input(0.1f, kEye, yaw + off, pitch, true, true, 0.008f).hits;
        hits += sim.Input(0.2f, kEye, yaw, pitch + off, true, true, 0.008f).hits;
    }
    CHECK(hits == 4);
    int misses = 0;
    for (float off : {outside, -outside}) {
        misses += sim.Input(0.3f, kEye, yaw + off, pitch, true, true, 0.008f).misses;
        misses += sim.Input(0.4f, kEye, yaw, pitch + off, true, true, 0.008f).misses;
    }
    CHECK(misses == 4);
}

TEST("perfect tracking scores the same at 30, 125, 1000 Hz; a frame spike credits at most 0.05 s") {
    const Scenario sc = Make(R"({"duration": 10, "fire": "hold", "targets": {"alive": 1, "radius": 16, "health": 1000,
        "spawn": {"distance": [500, 800], "yaw": [-40, 40], "pitch": [-5, 15]},
        "motion": {"type": "float", "speed": [200, 500], "changeEvery": [0.15, 0.6]}}})");
    const RunRecord r30 = TrackWithSteps(sc, {1 / 30.0f});
    const RunRecord r125 = TrackWithSteps(sc, {1 / 125.0f});
    const RunRecord r1000 = TrackWithSteps(sc, {1 / 1000.0f});
    const RunRecord spiky = TrackWithSteps(sc, {0.004f, 0.004f, 0.25f, 0.004f, 0.06f, 0.008f, 0.12f});
    for (const RunRecord* r : {&r30, &r125, &r1000, &spiky}) {
        CHECK_NEAR(r->summary.accuracy, 1.0, 1e-4);
    }
    // The aim-error samples sit on a fixed 20 Hz grid, so a perfectly tracked
    // run logs the same series at every command rate: 10 s / 0.05 s.
    const size_t samples = 200;
    CHECK(r30.shots.size() == samples);
    CHECK(r125.shots.size() == samples);
    CHECK(r1000.shots.size() == samples);
    // The score is on-target seconds x 100. A run stops issuing commands at the
    // last t before the duration and the sim credits nothing once it is
    // finished, so each rate can be short of the full 10 s by one frame at most.
    const float full = sc.duration * sc.score.perSecondOnTarget;
    CHECK_NEAR(r30.summary.score, full, 0.034f * sc.score.perSecondOnTarget);
    CHECK_NEAR(r125.summary.score, full, 0.009f * sc.score.perSecondOnTarget);
    CHECK_NEAR(r1000.summary.score, full, 0.002f * sc.score.perSecondOnTarget);
    // A hitch is not free: dt is clamped to 0.05 s per command, so the spiky run
    // is credited 3.748 s of its 10 s and cannot log the samples it slept through.
    CHECK_NEAR(spiky.summary.onTarget, 3.748f, 0.02f);
    CHECK(spiky.shots.size() < samples / 2);
}

TEST("a click after a long frame spike hits where the target is now") {
    const Scenario sc = Make(R"({"duration": 10, "targets": {"alive": 1, "radius": 16, "health": 1,
        "spawn": {"distance": 700, "yaw": [-30, 30], "pitch": [0, 10]},
        "motion": {"type": "strafe", "speed": [300, 300], "changeEvery": [0.2, 0.2]}}})");
    TrainerSim sim;
    sim.Start(sc, 4, kEye, 0);
    // 0.5 s without commands (a hitch), then one click aimed at the current position
    sim.Advance(0.5f);
    float yaw, pitch;
    AimAt(sim.TargetPosition(sim.Targets()[0]), yaw, pitch);
    const TrainerEvents ev = sim.Input(0.5f, kEye, yaw, pitch, true, true, 0.5f);
    CHECK(ev.hits == 1 && ev.kills == 1);
}

TEST("a timed-out target can't be hit, its replacement can") {
    const Scenario sc = Make(R"({"duration": 10, "targets": {"alive": 1, "radius": 16, "health": 1, "lifetime": 0.5,
        "spawn": {"distance": 700, "yaw": [-30, 30], "pitch": [0, 10]}}})");
    TrainerSim sim;
    sim.Start(sc, 8, kEye, 0);
    float yaw, pitch;
    AimAt(sim.TargetPosition(sim.Targets()[0]), yaw, pitch);  // the first target, which expires at 0.5 s
    TrainerEvents ev = sim.Input(0.6f, kEye, yaw, pitch, true, true, 0.008f);
    // the replacement spawned somewhere else, so the old spot is (almost surely) a miss
    CHECK(sim.Expired() == 1);
    AimAt(sim.TargetPosition(sim.Targets()[0]), yaw, pitch);
    ev = sim.Input(0.7f, kEye, yaw, pitch, true, true, 0.008f);
    CHECK(ev.kills == 1);
}

TEST("replays reproduce recorded runs through the saved file (rounded times)") {
    // a bot run on a timed, moving scenario, saved and loaded like a real record
    const Scenario sc = Make(R"({"duration": 20, "targets": {"alive": 2, "radius": 18, "health": 1, "lifetime": 1.2,
        "spawn": {"distance": [600, 900], "yaw": [-35, 35], "pitch": [-5, 15]},
        "motion": {"type": "strafe", "speed": [150, 300], "changeEvery": [0.3, 0.9]}}})");
    TrainerSim sim;
    sim.Start(sc, 2024, kEye, 0);
    for (int i = 1; !sim.Finished(); i++) {
        const float t = i * 0.008f;
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
        sim.Input(t, kEye, yaw, pitch, i % 25 == 0, i % 25 == 0, 0.008f);
    }
    RunRecord rec = sim.Record();
    RunRecord loaded;
    std::string error;
    CHECK(RecordFromJson(RecordToJson(rec), loaded, error));
    CHECK(loaded.kills.size() == rec.kills.size() && !rec.kills.empty());

    TrainerSim replay;
    replay.Start(sc, loaded.seed, kEye, 0);
    int replayed = 0;
    for (const KillEvent& k : loaded.kills) {
        replayed += replay.KillSerial(k.target, k.t) ? 1 : 0;
    }
    replay.Advance(sc.duration);
    CHECK(replayed == static_cast<int>(loaded.kills.size()));  // every recorded kill finds its target alive
    CHECK(replay.Expired() == sim.Expired());
    for (size_t i = 0; i < replay.Targets().size(); i++) {
        CHECK(replay.Targets()[i].serial == sim.Targets()[i].serial);
    }
}
