// Scenarios, benchmarks and run history from disk, and the insights over them.
//
// This file is part of Skaar. GPLv2 or later.
#include <algorithm>

#include "../shared/weapons.h"
#include "local.h"

namespace ui {
namespace {

std::string StripJson(const std::string& name) {
    // the engine lists files case-insensitively, so ".JSON" arrives here too
    return name.size() > 5 && Q_stricmp(name.c_str() + name.size() - 5, ".json") == 0 ? name.substr(0, name.size() - 5)
                                                                                      : name;
}

skaar::RunTime Now() {
    qtime_t t{};
    trap::RealTime(&t);
    skaar::RunTime r;
    r.year = t.tm_year + 1900;
    r.month = t.tm_mon + 1;
    r.day = t.tm_mday;
    r.hour = t.tm_hour;
    r.minute = t.tm_min;
    r.second = t.tm_sec;
    return r;
}

// Category order in lists: the built-in three first, then anything users add
int CategoryRank(const std::string& c) {
    if (c == "click") {
        return 0;
    }
    if (c == "track") {
        return 1;
    }
    if (c == "switch") {
        return 2;
    }
    return 3;
}

}  // namespace

const skaar::Scenario* Data::FindScenario(const std::string& id) const {
    for (const skaar::Scenario& s : scenarios) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

std::string Data::NameOf(const std::string& scenarioId) const {
    if (const skaar::Scenario* s = FindScenario(scenarioId)) {
        return s->name;
    }
    if (auto it = perScenario.find(scenarioId); it != perScenario.end()) {
        return it->second.name;
    }
    return scenarioId;
}

void LoadData() {
    const int start = trap::Milliseconds();
    Data d;
    d.now = Now();

    for (const std::string& file : trap::FileList("scenarios", ".json")) {
        const std::string id = StripJson(file);
        std::string text, error;
        skaar::Scenario s;
        if (!trap::FileRead(skaar::Va("scenarios/%s", file.c_str()), text)) {
            // listed but unreadable (permissions, a pure server): say so, don't drop it
            d.scenarioErrors.emplace_back(id, "could not be read");
            Com_Printf(S_COLOR_YELLOW "WARNING: scenarios/%s: could not be read\n", file.c_str());
            continue;
        }
        if (skaar::ParseScenario(id, text, s, error)) {
            d.scenarios.push_back(std::move(s));
        } else {
            d.scenarioErrors.emplace_back(id, error);
            Com_Printf(S_COLOR_YELLOW "WARNING: scenarios/%s: %s\n", file.c_str(), error.c_str());
        }
    }
    std::stable_sort(d.scenarios.begin(), d.scenarios.end(), [](const skaar::Scenario& a, const skaar::Scenario& b) {
        const int ra = CategoryRank(a.category), rb = CategoryRank(b.category);
        return ra != rb ? ra < rb : a.category != b.category ? a.category < b.category : a.name < b.name;
    });

    // weapon files: the game modules load these too, so register what parses here as
    // well (Insights › Arena charts skaar::Weapons()); errors show next to the scenario list
    skaar::ResetWeapons();
    for (const std::string& file : trap::FileList("weapons", ".json")) {
        const std::string id = StripJson(file);
        std::string text, error;
        skaar::WeaponDef w;
        if (!trap::FileRead(skaar::Va("weapons/%s", file.c_str()), text)) {
            d.scenarioErrors.emplace_back("weapons/" + id, "could not be read");
            Com_Printf(S_COLOR_YELLOW "WARNING: weapons/%s: could not be read\n", file.c_str());
            continue;
        }
        if (skaar::ParseWeapon(id, text, w, error)) {
            skaar::AddWeapon(w);
        } else {
            d.scenarioErrors.emplace_back("weapons/" + id, error);
        }
    }

    for (const std::string& file : trap::FileList("benchmarks", ".json")) {
        const std::string id = StripJson(file);
        std::string text, error;
        skaar::Benchmark b;
        if (!trap::FileRead(skaar::Va("benchmarks/%s", file.c_str()), text)) {
            d.benchmarkErrors.emplace_back(id, "could not be read");
            Com_Printf(S_COLOR_YELLOW "WARNING: benchmarks/%s: could not be read\n", file.c_str());
            continue;
        }
        if (skaar::ParseBenchmark(id, text, b, error)) {
            d.benchmarks.push_back(std::move(b));
        } else {
            d.benchmarkErrors.emplace_back(id, error);
            Com_Printf(S_COLOR_YELLOW "WARNING: benchmarks/%s: %s\n", file.c_str(), error.c_str());
        }
    }

    // a benchmark may name a scenario this pack doesn't ship. Such an entry can never be
    // played, so it would hold the focus pick (and leave the benchmark unranked) forever
    for (skaar::Benchmark& b : d.benchmarks) {
        for (auto it = b.entries.begin(); it != b.entries.end();) {
            if (d.FindScenario(it->scenario)) {
                ++it;
                continue;
            }
            d.benchmarkErrors.emplace_back(b.id, "unknown scenario " + it->scenario);
            Com_Printf(S_COLOR_YELLOW "WARNING: benchmarks/%s: unknown scenario '%s'\n", b.id.c_str(),
                       it->scenario.c_str());
            it = b.entries.erase(it);
        }
    }

    std::string history;
    if (trap::FileRead(skaar::kHistoryPath, history)) {
        d.history = skaar::ParseHistory(history, &d.historySkipped);
        if (d.historySkipped) {
            Com_Printf(S_COLOR_YELLOW "WARNING: %s: %d unreadable line(s) left out\n", skaar::kHistoryPath,
                       d.historySkipped);
        }
    }
    std::string arena;
    if (trap::FileRead(skaar::kArenaHistoryPath, arena)) {
        d.arena = skaar::ParseArenaHistory(arena);
    }
    d.perScenario = skaar::insights::PerScenario(d.history);
    d.overview = skaar::insights::Summarize(d.history, d.now);

    data = std::move(d);
    // progress keeps pointers into data.benchmarks, so build it after the move
    for (const skaar::Benchmark& b : data.benchmarks) {
        data.progress.push_back(skaar::insights::Progress(b, data.perScenario, data.history, data.scenarios));
    }
    Com_DPrintf("ui: loaded %zu scenarios, %zu runs in %d ms\n", data.scenarios.size(), data.history.size(),
                trap::Milliseconds() - start);
}

const skaar::RunRecord* LoadRunRecord(const std::string& file) {
    if (file.empty()) {
        return nullptr;
    }
    if (auto it = ui.records.find(file); it != ui.records.end()) {
        return &it->second;
    }
    std::string text, error;
    if (!trap::FileRead(file.c_str(), text)) {
        ui.runError = "The run file is missing: " + file;
        return nullptr;
    }
    skaar::RunRecord record;
    if (!skaar::RecordFromJson(text, record, error)) {
        ui.runError = "The run file can't be read: " + error;
        return nullptr;
    }
    if (ui.records.size() >= 8) {
        ui.records.erase(ui.records.begin());
    }
    return &ui.records.emplace(file, std::move(record)).first->second;
}

}  // namespace ui
