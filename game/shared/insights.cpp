// This file is part of Skaar. GPLv2 or later.
#include "insights.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace skaar::insights {
namespace {

float Mean(const std::vector<float>& v) {
    if (v.empty()) {
        return 0;
    }
    float sum = 0;
    for (float x : v) {
        sum += x;
    }
    return sum / static_cast<float>(v.size());
}

// History is a plain JSON file the player may hand-edit, so a duration is bounded
// before it reaches a cast or a total the screens format.
float SafeSeconds(float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 86400.0f) : 0; }

}  // namespace

bool IsPersonalBest(const std::vector<RunSummary>& history, int index) {
    const RunSummary& run = history[index];
    for (int i = 0; i < index; i++) {
        if (history[i].scenario == run.scenario && history[i].score >= run.score) {
            return false;
        }
    }
    return true;
}

std::map<std::string, ScenarioStats> PerScenario(const std::vector<RunSummary>& history) {
    std::map<std::string, ScenarioStats> out;
    for (int i = 0; i < static_cast<int>(history.size()); i++) {
        const RunSummary& r = history[i];
        ScenarioStats& s = out[r.scenario];
        s.id = r.scenario;
        s.name = r.name;  // newest name wins
        s.category = r.category;
        s.runs.push_back(i);
        s.seconds += r.duration;
        s.last = r.score;
        s.lastPlayed = std::max(s.lastPlayed, r.when.Seconds());
        if (s.bestRun < 0 || r.score > s.best) {
            s.best = r.score;
            s.bestRun = i;
        }
    }

    for (auto& [id, s] : out) {
        const int n = static_cast<int>(s.runs.size());
        std::vector<float> recent, previous, acc, ttk;
        for (int k = std::max(0, n - kRecent); k < n; k++) {
            const RunSummary& r = history[s.runs[k]];
            recent.push_back(r.score);
            acc.push_back(r.accuracy);
            if (r.kills > 0 && r.ttk > 0) {
                ttk.push_back(r.ttk);
            }
        }
        for (int k = std::max(0, n - 2 * kRecent); k < n - kRecent; k++) {
            previous.push_back(history[s.runs[k]].score);
        }
        s.recentAvg = Mean(recent);
        s.recentAccuracy = Mean(acc);
        s.recentTtk = Mean(ttk);
        const float prev = Mean(previous);
        s.trend = prev > 0 ? (s.recentAvg - prev) / prev : 0;
    }
    return out;
}

Overview Summarize(const std::vector<RunSummary>& history, const RunTime& now) {
    Overview o;
    o.totalRuns = static_cast<int>(history.size());
    if (history.empty()) {
        return o;
    }

    const int64_t today = now.DayNumber();
    std::set<int64_t> days;
    int64_t newest = -1;
    for (int i = 0; i < o.totalRuns; i++) {
        const RunSummary& r = history[i];
        o.totalSeconds += SafeSeconds(r.duration);
        days.insert(r.when.DayNumber());
        if (r.when.DayNumber() == today) {
            o.todayRuns++;
            o.todaySeconds += SafeSeconds(r.duration);
        }
        // history is append-ordered, but don't trust it for "latest"
        if (r.when.Seconds() >= newest) {
            newest = r.when.Seconds();
            o.latest = i;
        }
        if (today - r.when.DayNumber() < 7 && IsPersonalBest(history, i)) {
            o.weekPersonalBests++;
        }
    }
    o.playedToday = days.count(today) != 0;
    o.latestIsPb = IsPersonalBest(history, o.latest);

    // streak: walk back from today (or yesterday, so an evening without a run yet doesn't reset it)
    int64_t day = o.playedToday ? today : today - 1;
    while (days.count(day)) {
        o.streakDays++;
        day--;
    }

    // current session: newest runs with no gap longer than kSessionGap between them
    std::vector<int64_t> ends;
    for (const RunSummary& r : history) {
        ends.push_back(r.when.Seconds());
    }
    std::vector<int> order(history.size());
    for (int i = 0; i < o.totalRuns; i++) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return ends[a] > ends[b]; });
    int64_t prev = ends[order[0]];
    for (int i : order) {
        if (prev - ends[i] > kSessionGap) {
            break;
        }
        o.sessionRuns++;
        o.sessionSeconds += SafeSeconds(history[i].duration);
        prev = ends[i] - static_cast<int64_t>(SafeSeconds(history[i].duration));
    }
    return o;
}

BenchmarkProgress Progress(const Benchmark& benchmark, const std::map<std::string, ScenarioStats>& stats,
                           const std::vector<RunSummary>& history, const std::vector<Scenario>& scenarios) {
    BenchmarkProgress p;
    p.benchmark = &benchmark;
    const int tiers = static_cast<int>(benchmark.tiers.size());

    p.overallTier = tiers - 1;
    float sum = 0;
    std::map<std::string, std::vector<float>> byCategory;
    int bestPick = -1;
    float bestPickValue = 1e9f;
    int64_t bestPickPlayed = 0;

    for (int i = 0; i < static_cast<int>(benchmark.entries.size()); i++) {
        const Benchmark::Entry& entry = benchmark.entries[i];
        BenchmarkEntryProgress e;
        e.scenario = entry.scenario;
        int64_t lastPlayed = 0;
        if (auto it = stats.find(entry.scenario); it != stats.end()) {
            e.played = true;
            e.best = it->second.best;
            lastPlayed = it->second.lastPlayed;
        }
        e.tier = e.played ? TierFor(entry, e.best, &e.progress) : -1;
        p.overallTier = std::min(p.overallTier, e.tier);

        const float value = static_cast<float>(e.tier + 1) + (e.tier + 1 < tiers ? e.progress : 0);
        sum += value;

        std::string category;
        for (const Scenario& s : scenarios) {
            if (s.id == entry.scenario) {
                category = s.category;
            }
        }
        byCategory[category].push_back(value);

        // focus pick: lowest standing; on a tie, the one played least recently
        if (value < bestPickValue || (value == bestPickValue && lastPlayed < bestPickPlayed)) {
            bestPick = i;
            bestPickValue = value;
            bestPickPlayed = lastPlayed;
        }
        p.entries.push_back(e);
    }
    (void)history;

    if (!benchmark.entries.empty()) {
        p.overallScore = sum / static_cast<float>(benchmark.entries.size());
    }
    p.recommended = bestPick;

    float weakest = 1e9f;
    for (const auto& [category, values] : byCategory) {
        const float m = Mean(values);
        if (!category.empty() && m < weakest) {
            weakest = m;
            p.weakestCategory = category;
        }
    }
    if (benchmark.entries.empty()) {
        p.overallTier = -1;  // nothing to stand on: "unranked", per insights.h
    }
    return p;
}

}  // namespace skaar::insights
