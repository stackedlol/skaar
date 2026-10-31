// This file is part of Skaar. GPLv2 or later.
#include "check.h"
#include "shared/runlog.h"

using namespace skaar;

TEST("calendar day numbers") {
    CHECK(RunTime{1970, 1, 1, 0, 0, 0}.DayNumber() == 0);
    CHECK(RunTime{2000, 3, 1, 0, 0, 0}.DayNumber() == 11017);
    CHECK(RunTime{2024, 2, 29, 0, 0, 0}.DayNumber() == 19782);
    CHECK(RunTime{2026, 3, 1, 0, 0, 0}.DayNumber() - RunTime{2026, 2, 28, 0, 0, 0}.DayNumber() == 1);
    CHECK(RunTime{2026, 10, 22, 18, 30, 5}.Stamp() == "20261022-183005");
    RunTime t;
    CHECK(RunTime::FromIso("2026-10-22T18:30:05", t) && t.hour == 18 && t.second == 5);
    CHECK(!RunTime::FromIso("2026-13-01T00:00:00", t));
    CHECK(!RunTime::FromIso("yesterday", t));
    // every component, not just month/day: the accessors assume a real date
    CHECK(!RunTime::FromIso("2026-02-31T00:00:00", t));
    CHECK(!RunTime::FromIso("2026-10-22T700000:00:00", t));
    CHECK(!RunTime::FromIso("2026-10-22T-600000:00:00", t));
    CHECK(!RunTime::FromIso("-2147483648-01-01T00:00:00", t));
    CHECK(!RunTime::FromIso("2026-10-22T24:00:00", t));
    CHECK(!RunTime::FromIso("2026-10-22T00:60:00", t));
    CHECK(!RunTime::FromIso("2026-10-22T18:30:05junk", t));
    CHECK(RunTime::FromIso("2024-02-29T23:59:60", t));  // leap day and a leap second
    CHECK(t.DayNumber() == 19782);
    CHECK(RunTime{1970, 1, 1, 0, 0, 0}.Seconds() == 0);
    CHECK(RunTime{2026, 10, 22, 18, 30, 5}.Seconds() == 1792693805);
}

TEST("summary round trip") {
    RunSummary s;
    s.scenario = "wall-grid";
    s.name = "Wall grid";
    s.category = "click";
    s.when = {2026, 10, 22, 18, 30, 5};
    s.duration = 60;
    s.score = 12345.6f;
    s.shots = 140;
    s.hits = 130;
    s.kills = 130;
    s.accuracy = 130.0f / 140;
    s.ttk = 412.3f;
    s.sensCm360 = 34.6f;
    s.fov = 103;
    s.file = RecordPath(s);
    CHECK(s.file == "stats/runs/wall-grid__20261022-183005.json");

    RunSummary back;
    CHECK(SummaryFromJson(SummaryToJsonLine(s), back));
    CHECK(back.scenario == s.scenario && back.name == s.name && back.file == s.file);
    CHECK(back.when.Iso() == s.when.Iso());
    CHECK_NEAR(back.score, s.score, 0.05);
    CHECK_NEAR(back.accuracy, s.accuracy, 1e-4);
    CHECK(back.kills == 130 && back.shots == 140);
    CHECK_NEAR(back.sensCm360, 34.6, 0.01);
}

TEST("history numbers outside their type are skipped, not cast") {
    // 3e9 does not fit an int: the conversion used to be UB, then saturated
    int skipped = -1;
    auto runs = ParseHistory(
        R"({"scenario":"a","when":"2026-10-22T00:00:00","shots":3e9})" "\n"
        R"({"scenario":"b","when":"2026-10-22T00:00:00","hits":-1e10})" "\n"
        R"({"scenario":"c","when":"2026-10-22T00:00:00","kills":1e300})" "\n"
        R"({"scenario":"d","when":"2026-10-22T00:00:00","shots":2.5})" "\n"
        R"({"scenario":"e","when":"2026-10-22T00:00:00","shots":7})" "\n",
        &skipped);
    CHECK(runs.size() == 1 && runs[0].scenario == "e" && runs[0].shots == 7);
    CHECK(skipped == 4);
    // a record's seed and shot target get the same treatment
    RunRecord r;
    std::string error;
    CHECK(!RecordFromJson(R"({"summary": {"scenario": "a", "when": "2026-10-22T00:00:00"}, "seed": 1e300,
        "shots": [], "kills": [], "samples": []})", r, error));
    CHECK(!RecordFromJson(R"({"summary": {"scenario": "a", "when": "2026-10-22T00:00:00"}, "shots":
        [[1, 1, 1e300, 0, 0, 0]], "kills": [], "samples": []})", r, error));
}

TEST("a huge duration cannot reach the duration formatting") {
    // 1e30 used to reach ui::FormatDuration's static_cast<int> (UB, and 68 years in the tile)
    int skipped = -1;
    const auto runs = ParseHistory(
        R"({"scenario":"a","when":"2026-10-22T00:00:00","duration":1e30})" "\n"
        R"({"scenario":"b","when":"2026-10-22T00:00:00","duration":-5})" "\n",
        &skipped);
    CHECK(runs.size() == 2 && skipped == 0);
    for (const auto& r : runs) {
        CHECK(r.duration >= 0 && r.duration <= 86400 && static_cast<int>(r.duration) >= 0);
    }
}

TEST("history skips bad lines") {
    RunSummary a, b;
    a.scenario = "a";
    b.scenario = "b";
    const std::string text = SummaryToJsonLine(a) + "\n\n{broken\n" + R"({"no":"scenario"})" + "\n" +
                             SummaryToJsonLine(b) + "\r\n";
    int skipped = -1;
    const auto runs = ParseHistory(text, &skipped);
    CHECK(runs.size() == 2);
    CHECK(skipped == 2);  // "{broken" and the line without a scenario; blank lines don't count
    CHECK(runs.size() == 2 && runs[0].scenario == "a" && runs[1].scenario == "b");
}

TEST("record round trip") {
    RunRecord r;
    r.summary.scenario = "pop";
    r.summary.when = {2026, 1, 2, 3, 4, 5};
    r.seed = 4000000000u;
    r.shots.push_back({1.25f, true, 3, 0.5f, -0.25f, 1.1f});
    r.shots.push_back({2.5f, false, 4, -2.0f, 1.0f, 0.9f});
    r.kills.push_back({1.25f, 3, 512.5f});
    r.samples.push_back({0, 10.5f, -2.25f, 1, 2, 3});
    RunRecord back;
    std::string error;
    CHECK(RecordFromJson(RecordToJson(r), back, error));
    CHECK(back.seed == r.seed);
    CHECK(back.shots.size() == 2 && back.shots[0].hit && !back.shots[1].hit && back.shots[1].target == 4);
    CHECK(back.kills.size() == 1 && back.kills[0].target == 3);
    CHECK_NEAR(back.kills[0].ttk, 512.5, 0.05);
    CHECK(back.samples.size() == 1);
    CHECK_NEAR(back.samples[0].pitch, -2.25, 1e-3);
    CHECK(!RecordFromJson("{}", back, error) && !error.empty());
}

#include "shared/arenalog.h"

TEST("arena lines: no throw on bad bytes, no cast on bad numbers") {
    ArenaSession s;
    s.when = {2026, 10, 22, 20, 0, 0};
    s.map = "duel";
    s.ruleset = "cpm\xff";  // a config file is bytes, not UTF-8
    s.weapons["rail\xfe"] = {1, 2};
    const std::string line = ArenaSessionToJsonLine(s);  // used to throw type_error.316
    CHECK(!line.empty() && line.back() == '}');
    CHECK(ParseArenaHistory(line + "\n").size() == 1);  // and it is still readable JSON

    ArenaSession back;
    CHECK(!ArenaSessionFromJson(R"({"when":"2026-10-22T00:00:00","kills":1e300})", back));
    CHECK(!ArenaSessionFromJson(R"({"when":"2026-10-22T00:00:00","weapons":{"x":[1e300,2]}})", back));
    CHECK(!ArenaSessionFromJson(R"({"when":"2026-10-22T00:00:00","kills":1.5})", back));
    CHECK(ArenaSessionFromJson(R"({"when":"2026-10-22T00:00:00","kills":12,"weapons":{"x":[3,2]}})", back));
    CHECK(back.kills == 12 && back.weapons.at("x").shots == 3);
    // seconds is summed and formatted as a duration: 1e30 was UB in that int cast
    CHECK(ArenaSessionFromJson(R"({"when":"2026-10-22T00:00:00","seconds":1e30})", back));
    CHECK(back.seconds >= 0 && back.seconds <= 86400);
    CHECK(ArenaSessionFromJson(R"({"when":"2026-10-22T00:00:00","seconds":-30})", back));
    CHECK(back.seconds == 0);
}

TEST("arena session round trip") {
    ArenaSession s;
    s.when = {2026, 10, 22, 20, 0, 0};
    s.map = "duel";
    s.ruleset = "cpm";
    s.seconds = 312.4f;
    s.damageDealt = 4200;
    s.kills = 31;
    s.weapons["rail"] = {40, 22};
    s.weapons["rocket"] = {55, 30};
    const auto back = ParseArenaHistory(ArenaSessionToJsonLine(s) + "\n{bad\n");
    CHECK(back.size() == 1);
    if (!back.empty()) {
        CHECK(back[0].map == "duel" && back[0].ruleset == "cpm" && back[0].kills == 31);
        CHECK(back[0].weapons.at("rail").hits == 22);
        CHECK_NEAR(back[0].weapons.at("rocket").Accuracy(), 30.0 / 55, 1e-6);
    }
}
