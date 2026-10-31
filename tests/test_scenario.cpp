// This file is part of Skaar. GPLv2 or later.
#include <filesystem>
#include <fstream>
#include <sstream>

#include "check.h"
#include "shared/scenario.h"

using namespace skaar;

namespace {

std::string Slurp(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const std::filesystem::path kData = std::filesystem::path(SKAAR_SOURCE_DIR) / "data" / "base";

}  // namespace

TEST("every shipped scenario parses") {
    int n = 0;
    for (const auto& e : std::filesystem::directory_iterator(kData / "scenarios")) {
        Scenario s;
        std::string error;
        const bool ok = ParseScenario(e.path().stem().string(), Slurp(e.path()), s, error);
        if (!ok) {
            check::Fail(__FILE__, __LINE__, e.path().filename().string() + ": " + error);
        }
        n++;
    }
    CHECK(n >= 7);
}

TEST("every shipped benchmark parses and names real scenarios") {
    for (const auto& e : std::filesystem::directory_iterator(kData / "benchmarks")) {
        Benchmark b;
        std::string error;
        CHECK(ParseBenchmark(e.path().stem().string(), Slurp(e.path()), b, error));
        for (const auto& entry : b.entries) {
            if (!std::filesystem::exists(kData / "scenarios" / (entry.scenario + ".json"))) {
                check::Fail(__FILE__, __LINE__, b.id + " names missing scenario " + entry.scenario);
            }
        }
    }
}

TEST("scenario defaults, comments and ranges") {
    Scenario s;
    std::string error;
    CHECK(ParseScenario("x", R"({ // a comment
        "name": "X", "duration": 30,
        "targets": { "spawn": { "distance": [900, 700], "yaw": 20 } } })", s, error));
    CHECK(s.name == "X");
    CHECK(s.map == "range");
    CHECK(s.fire == FireMode::kClick);
    CHECK_NEAR(s.targets.distance.min, 700, 0);  // swapped into order
    CHECK_NEAR(s.targets.distance.max, 900, 0);
    CHECK_NEAR(s.targets.yaw.min, 20, 0);        // a number means [n, n]
    CHECK_NEAR(s.targets.yaw.max, 20, 0);
    CHECK(s.ruleset == "vq3" && !s.playerMove);
    CHECK(ParseScenario("y", R"({"player": {"move": true, "ruleset": "cpm"}})", s, error));
    CHECK(s.ruleset == "cpm" && s.playerMove);
}

TEST("bad scenarios are rejected with a reason") {
    const char* bad[] = {
        R"({"duration": 1})",
        R"({"targets": {"alive": 0}})",
        R"({"targets": {"radius": 0}})",
        R"({"fire": "burst"})",
        R"({"targets": {"alive": 9, "spawn": {"shape": "grid", "cols": 3, "rows": 3}}})",
        R"({"targets": {"spawn": {"distance": 10}}})",
        R"({"targets": {"motion": {"type": "zigzag"}}})",
        R"({"player": {"ruleset": "cpm; quit"}})",
        R"({"player": {"ruleset": ""}})",
        R"(not json)",
        // cols * rows used to overflow int (16777217 * 256 wraps to 256) and pass
        R"({"targets": {"spawn": {"cols": 16777217, "rows": 256}}})",
        R"({"targets": {"spawn": {"shape": "grid", "cols": 46341, "rows": 46341}}})",
        // a JSON number that is not a whole int: float->int was UB, then truncated
        R"({"targets": {"alive": 1e30}})",
        R"({"targets": {"alive": 3.9}})",
        R"({"targets": {"spawn": {"cols": 1e12, "rows": 1}}})",
        // pasted into "map %s": a separator would run the next console command
        R"({"map": "range; seta rcon_password pwned"})",
        R"({"map": "range\nquit"})",
        R"({"map": ""})",
        R"({"map": "ctf range"})",
        // a tiny lifetime stalls TrainerSim::Advance on its respawn loop
        R"({"targets": {"lifetime": 0.001}})",
        R"({"targets": {"lifetime": -3}})",
        // a non-object block was silently ignored, so the file loaded with defaults
        R"([1, 2, 3])",
        R"("hello")",
        R"(5)",
        R"(null)",
        R"({"targets": [{"alive": 4}]})",
        R"({"targets": 5})",
        R"({"targets": {"spawn": 7}})",
        R"({"score": "lot"})",
        R"({"player": []})",
    };
    for (const char* text : bad) {
        Scenario s;
        std::string error;
        if (ParseScenario("bad", text, s, error) || error.empty()) {
            check::Fail(__FILE__, __LINE__, std::string("accepted: ") + text);
        }
    }
}

TEST("an integral float is still a whole number") {
    // a hand-written file spells 4 as "4.0", and nlohmann truncated it before the
    // integer check went in, so only a real fraction may be rejected
    Scenario s;
    std::string error;
    CHECK(ParseScenario("x", R"({"duration": 60.0, "seed": 5.0,
        "targets": {"alive": 4.0, "radius": 16.0, "spawn": {"cols": 2.0, "rows": 3.0}}})", s, error));
    CHECK(s.targets.alive == 4 && s.targets.cols == 2 && s.targets.rows == 3);
    CHECK_NEAR(s.duration, 60, 1e-6);
    CHECK_NEAR(s.targets.radius, 16, 1e-6);
    CHECK(s.seed == 5u);
    CHECK(!ParseScenario("x", R"({"targets": {"alive": 4.5}})", s, error));
    CHECK(!ParseScenario("x", R"({"seed": 4294967296.0})", s, error));  // one past uint32_t
}

TEST("a map name may be mixed case, but not long or punctuated") {
    // community maps are named Ctf_Range and the engine's map search ignores case
    Scenario s;
    std::string error;
    CHECK(ParseScenario("x", R"({"map": "Ctf_Range"})", s, error));
    CHECK(s.map == "Ctf_Range");
    // "maps/<name>.bsp" has to fit MAX_QPATH, so 54 characters is the longest name
    const std::string longest(54, 'm');
    CHECK(ParseScenario("x", "{\"map\": \"" + longest + "\"}", s, error));
    CHECK(!ParseScenario("x", "{\"map\": \"" + longest + "m\"}", s, error));
}

TEST("benchmark tiers and progress") {
    Benchmark b;
    std::string error;
    CHECK(ParseBenchmark("b", R"({"tiers": ["a", "b", "c"], "scenarios": [{"id": "s", "thresholds": [10, 20, 40]}]})", b,
                         error));
    float p = 0;
    CHECK(TierFor(b.entries[0], 5, &p) == -1);
    CHECK_NEAR(p, 0.5, 1e-6);
    CHECK(TierFor(b.entries[0], 20, &p) == 1);
    CHECK_NEAR(p, 0.0, 1e-6);
    CHECK(TierFor(b.entries[0], 30, &p) == 1);
    CHECK_NEAR(p, 0.5, 1e-6);
    CHECK(TierFor(b.entries[0], 99, &p) == 2);
    CHECK_NEAR(p, 1.0, 1e-6);
    // thresholds must ascend and match the tiers
    CHECK(!ParseBenchmark("b", R"({"tiers": ["a", "b"], "scenarios": [{"id": "s", "thresholds": [20, 10]}]})", b, error));
    CHECK(!ParseBenchmark("b", R"({"tiers": ["a", "b"], "scenarios": [{"id": "s", "thresholds": [10]}]})", b, error));
}

TEST("lifetime: 0 means never, tiny nonzero values are rejected") {
    Scenario s;
    std::string error;
    CHECK(ParseScenario("x", R"({"targets": {"lifetime": 0}})", s, error));
    CHECK_NEAR(s.targets.lifetime, 0, 0);
    CHECK(ParseScenario("x", R"({"targets": {"lifetime": 0.05}})", s, error));
    CHECK(ParseScenario("x", R"({"targets": {"lifetime": 30}})", s, error));
    CHECK_NEAR(s.targets.lifetime, 30, 1e-6);
}

TEST("speed, health and the score weights are bounded") {
    Scenario s;
    std::string error;
    // a target that crosses its whole window between two Advance() calls would make the
    // motion depend on the frame rate, so the magnitude is capped
    CHECK(ParseScenario("x", R"({"targets": {"motion": {"type": "float", "speed": 5000}}})", s, error));
    CHECK(!ParseScenario("x", R"({"targets": {"motion": {"type": "float", "speed": 5001}}})", s, error));
    CHECK(!ParseScenario("x", R"({"targets": {"motion": {"type": "float", "speed": [-6000, 100]}}})", s, error));
    CHECK(ParseScenario("x", R"({"targets": {"health": 1000}})", s, error));
    CHECK(!ParseScenario("x", R"({"targets": {"health": 200000}})", s, error));
    CHECK(!ParseScenario("x", R"({"targets": {"spawn": {"distance": 200000}}})", s, error));
    CHECK(!ParseScenario("x", R"({"targets": {"motion": {"type": "strafe", "changeEvery": 61}}})", s, error));
    // 0..1: outside it the click score can go negative
    for (const char* w : {R"(-0.1)", R"(1.1)"}) {
        CHECK(!ParseScenario("x", std::string(R"({"score": {"accuracyWeight": )") + w + "}}", s, error));
    }
    CHECK(ParseScenario("x", R"({"score": {"accuracyWeight": 1}})", s, error));
    CHECK(!ParseScenario("x", R"({"score": {"kill": -100}})", s, error));
}

TEST("scenario JSON round trip (the in-game editor saves with it)") {
    for (const auto& e : std::filesystem::directory_iterator(kData / "scenarios")) {
        Scenario a, b;
        std::string error;
        CHECK(ParseScenario("x", Slurp(e.path()), a, error));
        CHECK(ParseScenario("x", ScenarioToJson(a), b, error));
        CHECK(ScenarioToJson(a) == ScenarioToJson(b));
        CHECK(a.name == b.name && a.fire == b.fire && a.targets.alive == b.targets.alive);
        CHECK(a.targets.motion == b.targets.motion && a.playerMove == b.playerMove && a.ruleset == b.ruleset);
        CHECK_NEAR(a.targets.speed.max, b.targets.speed.max, 1e-4);
    }
}

TEST("round trip keeps the fields no shape or motion needs") {
    // the writer used to omit cols/rows for a cone and speed/changeEvery for a static
    // target, so a save in the editor silently reset them
    Scenario a, b;
    std::string error;
    CHECK(ParseScenario("x", R"({"name": "Kept", "duration": 30,"targets": {
        "alive": 2, "radius": 20, "health": 3, "lifetime": 2.5,
        "spawn": {"shape": "cone", "cols": 7, "rows": 9, "distance": [500, 900]},
        "motion": {"type": "static", "speed": [100, 200], "changeEvery": [0.3, 0.9]}},
        "score": {"kill": 50, "accuracyWeight": 0.25, "perSecondOnTarget": 75}})", a, error));
    CHECK(ParseScenario("x", ScenarioToJson(a), b, error));
    CHECK(a.targets.cols == b.targets.cols && a.targets.rows == b.targets.rows);
    CHECK_NEAR(a.targets.lifetime, b.targets.lifetime, 0);
    CHECK(a.targets.motion == b.targets.motion && a.targets.shape == b.targets.shape);
    CHECK_NEAR(a.targets.speed.min, b.targets.speed.min, 1e-4);
    CHECK_NEAR(a.targets.changeEvery.max, b.targets.changeEvery.max, 1e-4);
    CHECK_NEAR(b.targets.speed.max, 200, 1e-4);
    CHECK_NEAR(a.score.accuracyWeight, b.score.accuracyWeight, 1e-6);
    // and a grid keeps its cells
    CHECK(ParseScenario("x", R"({"targets": {"alive": 4, "spawn": {"shape": "grid", "cols": 3, "rows": 4}}})", a,
                        error));
    CHECK(ParseScenario("x", ScenarioToJson(a), b, error));
    CHECK(b.targets.shape == SpawnShape::kGrid && b.targets.cols == 3 && b.targets.rows == 4);
}

TEST("saving a scenario whose name is not UTF-8 does not throw") {
    // a pack entry name is arbitrary bytes, and the name defaults to it
    Scenario s;
    std::string error;
    CHECK(ParseScenario("caf\xe9", "{}", s, error));
    CHECK(s.name.size() == 4);
    const std::string text = ScenarioToJson(s);  // used to throw type_error.316
    CHECK(!text.empty() && text.back() == '\n');
    Scenario back;
    CHECK(ParseScenario("caf\xe9", text, back, error));  // still readable JSON
}
