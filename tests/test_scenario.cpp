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
    // docs/scenarios.md's documented ranges hold exactly at the bounds; one step
    // outside is in the bad list below
    CHECK(ParseScenario("z", R"({"duration": 5, "targets": {"alive": 1, "radius": 1,
        "spawn": {"distance": 64}, "motion": {"type": "strafe", "changeEvery": 0.05}}})", s, error));
    CHECK_NEAR(s.targets.changeEvery.min, 0.05, 1e-6);
    CHECK(ParseScenario("z", R"({"duration": 600, "targets": {"alive": 16, "radius": 256,
        "spawn": {"distance": 64}}})", s, error));
}

TEST("bad scenarios are rejected with a reason") {
    const char* bad[] = {
        R"({"duration": 1})",
        R"({"duration": 601})",
        R"({"targets": {"alive": 0}})",
        R"({"targets": {"alive": 17}})",
        R"({"targets": {"radius": 0}})",
        R"({"targets": {"radius": 257}})",
        R"({"fire": "burst"})",
        R"({"targets": {"alive": 9, "spawn": {"shape": "grid", "cols": 3, "rows": 3}}})",
        R"({"targets": {"spawn": {"distance": 10}}})",
        R"({"targets": {"spawn": {"distance": 63}}})",
        R"({"targets": {"motion": {"type": "strafe", "changeEvery": 0.01}}})",
        R"({"targets": {"motion": {"type": "zigzag"}}})",
        R"({"player": {"ruleset": "cpm; quit"}})",
        R"({"player": {"ruleset": ""}})",
        R"(not json)",
    };
    for (const char* text : bad) {
        Scenario s;
        std::string error;
        if (ParseScenario("bad", text, s, error) || error.empty()) {
            check::Fail(__FILE__, __LINE__, std::string("accepted: ") + text);
        }
    }
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
