// This file is part of Skaar. GPLv2 or later.
#include "check.h"
#include "shared/insights.h"

using namespace skaar;
namespace ins = skaar::insights;

namespace {

RunSummary Run(const char* scenario, RunTime when, float score, float duration = 60) {
    RunSummary r;
    r.scenario = scenario;
    r.name = scenario;
    r.when = when;
    r.score = score;
    r.duration = duration;
    r.accuracy = 0.8f;
    r.kills = 10;
    r.ttk = 400;
    return r;
}

}  // namespace

TEST("streak counts consecutive days ending today or yesterday") {
    std::vector<RunSummary> h = {
        Run("a", {2026, 10, 18, 12, 0, 0}, 1),  // gap on the 19th
        Run("a", {2026, 10, 20, 12, 0, 0}, 1),
        Run("a", {2026, 10, 21, 23, 50, 0}, 1),
        Run("a", {2026, 10, 22, 0, 10, 0}, 1),
    };
    ins::Overview o = ins::Summarize(h, {2026, 10, 22, 20, 0, 0});
    CHECK(o.streakDays == 3 && o.playedToday && o.todayRuns == 1);
    o = ins::Summarize(h, {2026, 10, 23, 9, 0, 0});  // nothing yet today: the streak holds
    CHECK(o.streakDays == 3 && !o.playedToday);
    o = ins::Summarize(h, {2026, 10, 24, 9, 0, 0});  // a full day missed
    CHECK(o.streakDays == 0);
}

TEST("session ends at a 30 minute gap") {
    std::vector<RunSummary> h = {
        Run("a", {2026, 10, 22, 10, 0, 0}, 1),
        Run("a", {2026, 10, 22, 12, 0, 0}, 1),   // > 30 min after the last
        Run("a", {2026, 10, 22, 12, 10, 0}, 1),
        Run("a", {2026, 10, 22, 12, 25, 0}, 1),
    };
    const ins::Overview o = ins::Summarize(h, {2026, 10, 22, 13, 0, 0});
    CHECK(o.sessionRuns == 3);
    CHECK_NEAR(o.sessionSeconds, 180, 1e-3);
    CHECK(o.latest == 3);
}

TEST("a hostile duration does not poison the session totals") {
    // 1e30 does not fit int64: the cast was UB and the gap arithmetic overflowed
    std::vector<RunSummary> h = {Run("a", {2026, 10, 22, 10, 0, 0}, 1, 1e30f),
                                Run("b", {2026, 10, 22, 10, 1, 0}, 1, 60)};
    const ins::Overview o = ins::Summarize(h, {2026, 10, 22, 20, 0, 0});
    CHECK(std::isfinite(o.sessionSeconds) && o.sessionSeconds <= 2 * 86400);
    CHECK(std::isfinite(o.totalSeconds) && o.totalSeconds <= 2 * 86400);
    CHECK(o.sessionRuns == 2);
}

TEST("personal bests and trend") {
    std::vector<RunSummary> h;
    for (int i = 0; i < 20; i++) {
        h.push_back(Run("a", {2026, 10, 1 + i, 12, 0, 0}, i < 10 ? 100.0f : 110.0f));
    }
    h.push_back(Run("b", {2026, 10, 21, 12, 0, 0}, 5));
    const auto per = ins::PerScenario(h);
    const ins::ScenarioStats& a = per.at("a");
    CHECK(a.runs.size() == 20);
    CHECK_NEAR(a.best, 110, 1e-4);
    CHECK(a.bestRun == 10);  // the first 110; later equal scores aren't new bests
    CHECK_NEAR(a.trend, 0.10, 1e-4);
    CHECK(ins::IsPersonalBest(h, 0) && ins::IsPersonalBest(h, 10) && !ins::IsPersonalBest(h, 11));
    CHECK(ins::IsPersonalBest(h, 20));  // first run of b
}

TEST("benchmark standing and focus pick") {
    Benchmark b;
    std::string error;
    CHECK(ParseBenchmark("core", R"({"tiers": ["t1", "t2", "t3"], "scenarios": [
        {"id": "a", "thresholds": [10, 20, 30]},
        {"id": "b", "thresholds": [10, 20, 30]},
        {"id": "c", "thresholds": [10, 20, 30]}]})", b, error));
    std::vector<RunSummary> h = {Run("a", {2026, 10, 22, 10, 0, 0}, 25), Run("b", {2026, 10, 22, 11, 0, 0}, 12)};
    std::vector<Scenario> scenarios(3);
    scenarios[0].id = "a";
    scenarios[0].category = "click";
    scenarios[1].id = "b";
    scenarios[1].category = "track";
    scenarios[2].id = "c";
    scenarios[2].category = "track";

    ins::BenchmarkProgress p = ins::Progress(b, ins::PerScenario(h), h, scenarios);
    CHECK(p.entries[0].tier == 1 && p.entries[1].tier == 0 && p.entries[2].tier == -1);
    CHECK(p.overallTier == -1);    // c is unplayed
    CHECK(p.recommended == 2);     // so it's the focus
    CHECK(p.weakestCategory == "track");

    // a benchmark that lists no scenarios has nothing to stand on: -1, not the top tier
    Benchmark empty;
    CHECK(ParseBenchmark("e", R"({"tiers": ["t1", "t2", "t3"], "scenarios": []})", empty, error));
    const ins::BenchmarkProgress pe = ins::Progress(empty, ins::PerScenario(h), h, scenarios);
    CHECK(pe.entries.empty() && pe.overallTier == -1 && pe.overallScore == 0);

    h.push_back(Run("c", {2026, 10, 22, 12, 0, 0}, 31));
    p = ins::Progress(b, ins::PerScenario(h), h, scenarios);
    CHECK(p.overallTier == 0);     // the lowest entry's tier
    CHECK(p.recommended == 1);     // b: tier 0, 20% to t2
    CHECK_NEAR(p.overallScore, ((2 + 0.5) + (1 + 0.2) + 3) / 3.0, 1e-4);
}
