// Insights: what your runs say. Overview (streak, session, latest run, focus),
// Scenarios (score / accuracy / TTK over runs), Run (one run in depth: TTK,
// shot dispersion, aim path, replay) and Benchmarks (tiers, what to play next).
//
// This file is part of Skaar. GPLv2 or later.
#include <algorithm>
#include <cmath>

#include "../shared/trainer_sim.h"
#include "../shared/weapons.h"
#include "local.h"

namespace ui {
namespace {

using skaar::RunSummary;
namespace insights = skaar::insights;

const Color kDot = theme::kCream.Alpha(0.55f);

float Mean(const std::vector<float>& v) {
    float s = 0;
    for (float x : v) {
        s += x;
    }
    return v.empty() ? 0 : s / v.size();
}

std::string Pct(float f) { return skaar::Va("%.1f%%", f * 100); }

// A record's times come from a file anyone can edit. Keep one inside the run before
// it becomes a sample index, an allocation size or a plot width.
float RunTime(float t, float duration) { return std::isfinite(t) ? std::clamp(t, 0.0f, duration) : 0.0f; }

// The length of a record, bounded: docs/scenarios.md allows 5..600 seconds
float RunDuration(float duration) { return std::isfinite(duration) ? std::clamp(duration, 1.0f, 600.0f) : 1.0f; }

bool IsHold(const std::string& scenarioId) {
    const skaar::Scenario* s = data.FindScenario(scenarioId);
    return s && s->fire == skaar::FireMode::kHold;
}

std::string ScenarioName(const RunSummary& r) { return r.name.empty() ? r.scenario : r.name; }

// "Personal best." / "312 below your best." for one run
std::string Verdict(int index) {
    const RunSummary& r = data.history[index];
    const auto it = data.perScenario.find(r.scenario);
    if (it == data.perScenario.end()) {
        return "";
    }
    if (it->second.bestRun == index) {
        return it->second.runs.size() == 1 ? "First run on this scenario." : "Personal best.";
    }
    const float avg = it->second.recentAvg;
    if (r.score >= avg) {
        return skaar::Va("%.0f above your recent average, %.0f below your best.", r.score - avg, it->second.best - r.score);
    }
    return skaar::Va("%.0f below your recent average of %.0f.", avg - r.score, avg);
}

int IndexOfFile(const std::string& file) {
    for (int i = static_cast<int>(data.history.size()) - 1; i >= 0; i--) {
        if (data.history[i].file == file) {
            return i;
        }
    }
    return -1;
}

void EmptyState(float top) {
    const kit::Fonts& f = kit::GetFonts();
    const float m = theme::kMargin;
    f.display.Draw(m - 4, top + 90, type::kTitle, "NO RUNS YET.", theme::kCream, theme::kTrackStatement);
    Paragraph(m, top + 120, 760,
              "Play a scenario. Every run you finish lands here: your streak, how today's session went, how each "
              "scenario trends, where your shots miss, and what to train next.",
              type::kBody, theme::kCreamSoft);
    if (ActionButton({m, top + 280, 420, 72}, "Pick a scenario", "ENTER")) {
        Open(Screen::kPlay);
    }
}

// ---- overview ----------------------------------------------------------------------

void DrawRecentTable(float x, float y, float w, int rows) {
    const kit::Fonts& f = kit::GetFonts();
    const float colScenario = x + 260, colScore = x + w * 0.58f, colAcc = x + w * 0.74f, colTtk = x + w - 20;
    const float rowH = 50;
    Label(x, y, "RECENT RUNS");
    Label(colScore, y, "SCORE", theme::kCreamFaint, Align::kRight);
    Label(colAcc, y, "ACCURACY", theme::kCreamFaint, Align::kRight);
    Label(colTtk, y, "MEAN TTK", theme::kCreamFaint, Align::kRight);
    y += 20;
    kit::HLine(x, y, w, theme::kHairline);
    int shown = 0;
    for (int i = static_cast<int>(data.history.size()) - 1; i >= 0 && shown < rows; i--, shown++) {
        const RunSummary& r = data.history[i];
        const Rect row{x - 40, y, w + 40, rowH};
        const int id = Focusable(row);
        const bool focused = Focused(id);
        const float baseline = y + rowH / 2 + 8;
        if (focused) {
            Marker(x - 28, baseline, 16);
        }
        const Color c = focused ? theme::kCream : theme::kCreamSoft;
        Label(x, baseline - 2, FormatDate(r.when, data.now), theme::kCreamFaint);
        f.text.Draw(colScenario, baseline, 22, ScenarioName(r), c);
        f.mono.Draw(colScore, baseline, 22, skaar::Va("%.0f", r.score), c, 0.01f, Align::kRight);
        f.mono.Draw(colAcc, baseline, 22, Pct(r.accuracy), c, 0.01f, Align::kRight);
        f.mono.Draw(colTtk, baseline, 22, r.ttk > 0 ? skaar::Va("%.0f MS", r.ttk) : "—", c, 0.01f, Align::kRight);
        kit::HLine(x, y + rowH, w, theme::kHairline);
        if (Activated(id, row) && !r.file.empty()) {
            ShowRun(r.file);
        }
        y += rowH;
    }
}

void DrawOverview(float top) {
    const kit::Fonts& f = kit::GetFonts();
    const float m = theme::kMargin, w = kit::Width() - 2 * m;
    const insights::Overview& o = data.overview;
    if (o.totalRuns == 0) {
        EmptyState(top);
        return;
    }

    // at a glance: big value, a second line under it
    const float cellW = w / 5, cellH = 136;
    CellGrid(m, top, cellW, cellH, 5, 5);
    auto glance = [&](int i, const char* label, const std::string& value, const std::string& sub, Color c) {
        const Rect r{m + i * cellW, top, cellW, cellH};
        Cell(r, label, value, c, 34);
        Label(r.x + 22, r.y + 116, sub, theme::kCreamFaint);
    };
    glance(0, "STREAK", skaar::Va("%d %s", o.streakDays, o.streakDays == 1 ? "DAY" : "DAYS"),
           o.playedToday ? "PLAYED TODAY" : (o.streakDays > 0 ? "PLAY TODAY TO KEEP IT" : "START ONE TODAY"),
           o.playedToday ? theme::kCream : theme::kCreamSoft);
    glance(1, "TODAY", skaar::Va("%d %s", o.todayRuns, o.todayRuns == 1 ? "RUN" : "RUNS"),
           FormatDuration(o.todaySeconds) + " PLAYED", theme::kCream);
    glance(2, "THIS SESSION", skaar::Va("%d %s", o.sessionRuns, o.sessionRuns == 1 ? "RUN" : "RUNS"),
           FormatDuration(o.sessionSeconds) + " PLAYED", theme::kCream);
    glance(3, "ALL TIME", skaar::Va("%d RUNS", o.totalRuns), FormatDuration(o.totalSeconds) + " PLAYED", theme::kCream);
    glance(4, "PERSONAL BESTS", skaar::Va("%d", o.weekPersonalBests), "IN THE LAST 7 DAYS", theme::kCream);

    // latest run
    const float blockY = top + cellH + 60;
    const RunSummary& latest = data.history[o.latest];
    Label(m, blockY, "LATEST RUN   ·   " + FormatDate(latest.when, data.now));
    f.text.Draw(m, blockY + 46, 30, ScenarioName(latest), theme::kCream);
    f.display.Draw(m - 6, blockY + 150, type::kBigNumber, skaar::Va("%.0f", latest.score),
                   o.latestIsPb ? kEmber : theme::kCream, -0.02f);
    f.text.Draw(m, blockY + 196, 22, Verdict(o.latest), theme::kCreamSoft);
    Label(m, blockY + 238,
          skaar::Va("%s %s   ·   %d KILLS   ·   TTK %s", IsHold(latest.scenario) ? "ON TARGET" : "ACCURACY",
                    Pct(latest.accuracy).c_str(), latest.kills,
                    latest.ttk > 0 ? skaar::Va("%.0f MS", latest.ttk) : "—"),
          theme::kCreamSoft);

    // benchmark focus
    const float bx = m + w * 0.52f, bw = w * 0.48f;
    if (!data.progress.empty()) {
        const insights::BenchmarkProgress& p = data.progress.front();
        const skaar::Benchmark& b = *p.benchmark;
        Label(bx, blockY, "BENCHMARK   ·   " + b.name);
        const std::string standing = p.overallTier >= 0 ? b.tiers[p.overallTier] : "Unranked";
        f.display.Draw(bx - 4, blockY + 96, 64, kit::Upper(standing), theme::kCream, theme::kTrackStatement);
        chart::Progress({bx, blockY + 122, bw * 0.8f, 3}, p.overallScore / b.tiers.size(), theme::kHairlineStrong,
                        theme::kCreamSoft);
        Label(bx + bw * 0.8f + 16, blockY + 128, skaar::Va("%.1f / %zu", p.overallScore, b.tiers.size()),
              theme::kCreamFaint);

        if (p.recommended >= 0) {
            const insights::BenchmarkEntryProgress& e = p.entries[p.recommended];
            Label(bx, blockY + 180, "FOCUS NEXT");
            const std::string why =
                !e.played ? "Not played yet."
                : e.tier < 0
                    ? skaar::Va("Below %s. %.0f%% of the way there.", b.tiers[0].c_str(), e.progress * 100)
                    : e.tier + 1 < static_cast<int>(b.tiers.size())
                          ? skaar::Va("%s now, %.0f%% to %s.", b.tiers[e.tier].c_str(), e.progress * 100,
                                      b.tiers[e.tier + 1].c_str())
                          : "Every tier cleared.";
            const Rect r{bx, blockY + 200, bw, 72};
            if (ActionButton(r, data.NameOf(e.scenario), why)) {
                StartScenario(e.scenario);
            }
            if (!p.weakestCategory.empty()) {
                Label(bx, blockY + 306, "WEAKEST AREA  ·  " + p.weakestCategory, theme::kCreamFaint);
            }
        }
    }

    // open latest
    const Rect open{m, blockY + 266, 360, 56};
    if (!latest.file.empty() && ActionButton(open, "Open this run", "→")) {
        ShowRun(latest.file);
    }

    DrawRecentTable(m, blockY + 368, w, 3);
}

// ---- scenarios -----------------------------------------------------------------------

// Small labelled metric curve over the runs
void MetricChart(const Rect& r, const char* label, const std::vector<float>& values, const char* format, bool lowerIsBetter) {
    Label(r.x - 60, r.y - 16, label);
    if (values.size() < 2) {
        kit::GetFonts().text.Draw(r.x - 60, r.y + 40, 20, "Needs two runs.", theme::kCreamFaint);
        return;
    }
    const auto [mn, mx] = std::minmax_element(values.begin(), values.end());
    float lo, hi, step;
    chart::NiceRange(*mn, *mx, 2, lo, hi, step);
    chart::YAxis(r, lo, hi, step, format);
    chart::Curve(r, values, lo, hi, theme::kCreamSoft, 1.5f);
    // newest value, right of the curve
    const float last = values.back(), first = values.front();
    const bool better = lowerIsBetter ? last < first : last > first;
    kit::GetFonts().mono.Draw(r.Right(), r.y - 16, 18, skaar::Va(format, last),
                              better ? theme::kCream : theme::kCreamSoft, 0.01f, Align::kRight);
}

void DrawScenarios(float top) {
    const kit::Fonts& f = kit::GetFonts();
    const float m = theme::kMargin, w = kit::Width(), h = kit::Height();
    if (data.perScenario.empty()) {
        EmptyState(top);
        return;
    }

    // played scenarios, most played first
    std::vector<const insights::ScenarioStats*> list;
    for (const auto& [id, s] : data.perScenario) {
        list.push_back(&s);
    }
    std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->lastPlayed > b->lastPlayed; });
    if (std::none_of(list.begin(), list.end(), [](auto* s) { return s->id == ui.selectedScenario; })) {
        ui.selectedScenario = list.front()->id;
    }

    const float listW = 480, rowH = 70;
    // a whole number of rows tall, so a row is never half in view
    const float listH = std::floor((h - top - 130) / rowH) * rowH;
    const Rect view{m - 40, top, listW + 40, listH};
    const int firstId = ui.focusCount;
    float focusTop = -1, focusBottom = -1;
    for (size_t i = 0; i < list.size(); i++) {
        if (ui.focus == firstId + static_cast<int>(i)) {
            focusTop = i * rowH;
            focusBottom = focusTop + rowH;
        }
    }
    const float scroll = Scroll("insights-scenarios", view, list.size() * rowH, focusTop, focusBottom, rowH);
    for (size_t i = 0; i < list.size(); i++) {
        const insights::ScenarioStats& s = *list[i];
        const float y = top + i * rowH - scroll;
        const Rect r{m - 40, y, listW + 40, rowH};
        if (y < view.y - 1 || y + rowH > view.Bottom() + 1) {
            ui.focusCount++;
            continue;
        }
        const int id = Focusable(r);
        if (Focused(id)) {
            ui.selectedScenario = s.id;
        }
        const bool selected = s.id == ui.selectedScenario;
        const float baseline = y + 32;
        if (Focused(id)) {
            Marker(m - 28, baseline, 18);
        }
        f.text.Draw(m, baseline, 24, data.NameOf(s.id), selected ? theme::kCream : theme::kCreamSoft);
        Label(m, baseline + 26, skaar::Va("%zu RUNS   ·   BEST %.0f", s.runs.size(), s.best), theme::kCreamFaint);
        kit::HLine(m, y + rowH, listW, theme::kHairline);
        if (Activated(id, r)) {
            ui.selectedScenario = s.id;
        }
    }

    const auto it = data.perScenario.find(ui.selectedScenario);
    if (it == data.perScenario.end()) {
        return;
    }
    const insights::ScenarioStats& s = it->second;
    const float x = m + listW + 100, pw = w - m - x;

    f.display.Draw(x - 4, top + 56, 56, kit::Upper(data.NameOf(s.id)), theme::kCream, theme::kTrackStatement);
    const float cellW = pw / 5, cellH = 104, cy = top + 86;
    CellGrid(x, cy, cellW, cellH, 5, 5);
    Cell({x, cy, cellW, cellH}, "BEST", skaar::Va("%.0f", s.best));
    Cell({x + cellW, cy, cellW, cellH}, "AVG · LAST 10", skaar::Va("%.0f", s.recentAvg));
    Cell({x + 2 * cellW, cy, cellW, cellH}, "TREND",
         s.runs.size() > static_cast<size_t>(insights::kRecent) ? skaar::Va("%+.1f%%", s.trend * 100) : "—",
         s.trend > 0.005f ? theme::kCream : theme::kCreamSoft);
    Cell({x + 3 * cellW, cy, cellW, cellH}, "RUNS", skaar::Va("%zu", s.runs.size()));
    Cell({x + 4 * cellW, cy, cellW, cellH}, "TIME PLAYED", FormatDuration(s.seconds));

    // score over runs; dots open the run
    const float chartTop = cy + cellH + 70;
    Label(x, chartTop - 22, "SCORE PER RUN   ·   CLICK A POINT TO OPEN THE RUN");
    ScoreChart({x + 70, chartTop, pw - 70, 250}, s.runs);

    // accuracy and TTK
    std::vector<float> acc, ttk;
    const size_t from = s.runs.size() > 60 ? s.runs.size() - 60 : 0;
    for (size_t k = from; k < s.runs.size(); k++) {
        const RunSummary& r = data.history[s.runs[k]];
        acc.push_back(r.accuracy * 100);
        if (r.ttk > 0) {
            ttk.push_back(r.ttk);
        }
    }
    const float smallTop = chartTop + 250 + 80, smallW = (pw - 70 * 2 - 60) / 2;
    MetricChart({x + 70, smallTop, smallW, 120}, IsHold(s.id) ? "ON TARGET %" : "ACCURACY %", acc, "%.0f", false);
    MetricChart({x + 70 + smallW + 130, smallTop, smallW, 120}, "MEAN TTK · MS", ttk, "%.0f", true);
}

// ---- run -----------------------------------------------------------------------------

// Rolling mean TTK over the last 8 kills, one value per kill
std::vector<float> RollingTtk(const skaar::RunRecord& rec) {
    std::vector<float> rolling;
    for (size_t i = 0; i < rec.kills.size(); i++) {
        const size_t a = i >= 7 ? i - 7 : 0;
        float sum = 0;
        for (size_t j = a; j <= i; j++) {
            sum += rec.kills[j].ttk;
        }
        rolling.push_back(sum / (i - a + 1));
    }
    return rolling;
}

// Share of 20 Hz aim samples on target, in 2 s windows
std::vector<float> OnTargetCurve(const skaar::RunRecord& rec, float duration) {
    const int buckets = std::clamp(static_cast<int>(RunDuration(duration) / 2), 2, 300);
    std::vector<float> on(buckets, 0), all(buckets, 0), pct;
    for (const skaar::ShotEvent& s : rec.shots) {
        const int b = std::clamp(static_cast<int>(RunTime(s.t, duration) / duration * buckets), 0, buckets - 1);
        all[b]++;
        on[b] += s.hit ? 1 : 0;
    }
    for (int b = 0; b < buckets; b++) {
        pct.push_back(all[b] > 0 ? on[b] / all[b] * 100 : 0);
    }
    return pct;
}

void DrawTtkPanel(const Rect& r, const skaar::RunRecord& rec, bool hold, const skaar::RunRecord* other) {
    Label(r.x, r.y, hold ? "ON TARGET OVER THE RUN" : "TIME TO KILL OVER THE RUN");
    const Rect plot{r.x + 60, r.y + 40, r.w - 60, r.h - 80};
    const float duration = RunDuration(rec.summary.duration);

    if (hold) {
        chart::YAxis(plot, 0, 100, 25, "%.0f");
        if (other) {
            chart::Curve(plot, OnTargetCurve(*other, duration), 0, 100, theme::kCreamFaint, 1.0f);
        }
        chart::Curve(plot, OnTargetCurve(rec, duration), 0, 100, theme::kCream, 1.5f, theme::kCream.Alpha(0.05f));
    } else {
        if (rec.kills.empty()) {
            kit::GetFonts().text.Draw(plot.x, plot.y + 40, 20, "No kills in this run.", theme::kCreamFaint);
            return;
        }
        std::vector<float> ttks;
        for (const skaar::KillEvent& k : rec.kills) {
            ttks.push_back(k.ttk);
        }
        std::vector<float> sorted = ttks;
        std::sort(sorted.begin(), sorted.end());
        const float p95 = sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)];
        float lo, hi, step;
        chart::NiceRange(0, p95, 3, lo, hi, step);
        chart::YAxis(plot, lo, hi, step, "%.0f");
        std::vector<float> dots;
        for (const skaar::KillEvent& k : rec.kills) {
            dots.push_back(plot.x + RunTime(k.t, duration) / duration * plot.w);
            dots.push_back(plot.Bottom() - std::min(k.ttk, hi) / hi * plot.h);
        }
        chart::Dots(dots, 5, kDot);
        if (other && !other->kills.empty()) {
            // the comparison run's rolling mean, faint, on the same scale and time axis
            const float d = RunDuration(other->summary.duration);
            const float ox0 = plot.x + RunTime(other->kills.front().t, d) / d * plot.w;
            const float ox1 = plot.x + RunTime(other->kills.back().t, d) / d * plot.w;
            chart::Curve({ox0, plot.y, std::max(1.0f, ox1 - ox0), plot.h}, RollingTtk(*other), lo, hi,
                         theme::kCreamFaint, 1.0f);
        }
        const std::vector<float> rolling = RollingTtk(rec);
        const float x0 = plot.x + RunTime(rec.kills.front().t, duration) / duration * plot.w;
        const float x1 = plot.x + RunTime(rec.kills.back().t, duration) / duration * plot.w;
        chart::Curve({x0, plot.y, std::max(1.0f, x1 - x0), plot.h}, rolling, lo, hi, theme::kCream, 2.0f);
        // first vs last third: do you slow down?
        const size_t third = ttks.size() / 3;
        if (third >= 3) {
            const float early = Mean(std::vector<float>(ttks.begin(), ttks.begin() + third));
            const float late = Mean(std::vector<float>(ttks.end() - third, ttks.end()));
            const float change = (late - early) / early;
            std::string text = std::fabs(change) < 0.05f ? "Steady pace from start to finish."
                               : change > 0 ? skaar::Va("Last third %.0f%% slower than the first.", change * 100)
                                            : skaar::Va("Last third %.0f%% faster than the first.", -change * 100);
            kit::GetFonts().text.Draw(r.x, r.Bottom() + 20, 20, text, theme::kCreamSoft);
        }
    }
    Label(plot.x, plot.Bottom() + 24, other ? "0:00   ·   FAINT = COMPARED RUN" : "0:00", theme::kCreamFaint);
    Label(plot.Right(), plot.Bottom() + 24, FormatDuration(duration), theme::kCreamFaint, Align::kRight);
}

void DrawDispersionPanel(const Rect& r, const skaar::RunRecord& rec, bool hold) {
    Label(r.x, r.y, hold ? "AIM ERROR WHILE FIRING" : "SHOT DISPERSION");
    const float radius = std::min(r.w, r.h - 100) / 2;
    const float cx = r.x + r.w / 2, cy = r.y + 40 + radius;
    const float span = 2.5f;  // target radii to the plot edge
    chart::Ring(cx, cy, radius / span, theme::kCreamFaint, 1);
    chart::Ring(cx, cy, radius, theme::kHairlineStrong, 1);
    kit::HLine(cx - radius, cy, radius * 2, theme::kHairline);
    kit::VLine(cx, cy - radius, radius * 2, theme::kHairline);
    // every shot counts toward the numbers; at most ~1200 are drawn (the render queue is finite)
    float sx = 0, sy = 0;
    int n = 0, inside = 0;
    const size_t stride = std::max<size_t>(1, rec.shots.size() / 1200);
    std::vector<float> hitDots, missDots;
    for (size_t i = 0; i < rec.shots.size(); i++) {
        const skaar::ShotEvent& s = rec.shots[i];
        if (s.radius <= 0) {
            continue;
        }
        const float ux = s.dx / s.radius, uy = s.dy / s.radius;
        sx += ux;
        sy += uy;
        inside += s.hit ? 1 : 0;
        n++;
        if (i % stride || std::fabs(ux) > span || std::fabs(uy) > span) {
            continue;  // thinned, or off the plot
        }
        std::vector<float>& dots = s.hit ? hitDots : missDots;
        dots.push_back(cx + ux / span * radius);
        dots.push_back(cy - uy / span * radius);
    }
    chart::Dots(missDots, 4, kEmber.Alpha(0.85f));
    chart::Dots(hitDots, 4, kDot);
    if (n == 0) {
        return;
    }
    sx /= n;
    sy /= n;
    // mean point of impact
    chart::Ring(cx + std::clamp(sx / span, -1.0f, 1.0f) * radius, cy - std::clamp(sy / span, -1.0f, 1.0f) * radius, 7,
                theme::kCream, 2);
    const char* hDir = sx > 0.08f ? "right" : sx < -0.08f ? "left" : "";
    const char* vDir = sy > 0.08f ? "high" : sy < -0.08f ? "low" : "";
    const std::string bias = (*vDir || *hDir)
                                 ? skaar::Va("Shots drift %s%s%s, %.2f radii off centre.", vDir, (*vDir && *hDir) ? " and " : "",
                                             hDir, std::hypot(sx, sy))
                                 : std::string("Shots centre on the target.");
    kit::GetFonts().text.Draw(r.x, cy + radius + 44, 20, bias, theme::kCreamSoft);
    Label(r.x, cy + radius + 76,
          skaar::Va("INNER RING = TARGET EDGE   ·   %.0f%% INSIDE   ·   RING = MEAN", 100.0f * inside / n),
          theme::kCreamFaint);
}

void DrawPathPanel(const Rect& r, const skaar::RunRecord& rec) {
    Label(r.x, r.y, "AIM PATH   ·   VIEW ANGLES OVER THE RUN");
    const auto& s = rec.samples;
    if (s.size() < 2) {
        kit::GetFonts().text.Draw(r.x, r.y + 60, 20, "No view samples in this run.", theme::kCreamFaint);
        return;
    }
    // bounds of yaw (relative to the first sample) and pitch
    const float yaw0 = s.front().yaw;
    float minYaw = 0, maxYaw = 0, minPitch = s.front().pitch, maxPitch = minPitch;
    for (const skaar::ViewSample& v : s) {
        const float y = skaar::AngleDelta(v.yaw, yaw0);
        minYaw = std::min(minYaw, y);
        maxYaw = std::max(maxYaw, y);
        minPitch = std::min(minPitch, v.pitch);
        maxPitch = std::max(maxPitch, v.pitch);
    }
    const Rect plot{r.x, r.y + 30, r.w, r.h * 0.62f};
    // equal degrees per unit on both axes, centred
    const float spanYaw = std::max(1.0f, maxYaw - minYaw), spanPitch = std::max(1.0f, maxPitch - minPitch);
    const float k = std::min(plot.w / spanYaw, plot.h / spanPitch);
    const float ox = plot.x + plot.w / 2, oy = plot.y + plot.h / 2;
    const float midYaw = (minYaw + maxYaw) / 2, midPitch = (minPitch + maxPitch) / 2;
    auto toX = [&](float yawRel) { return ox - (yawRel - midYaw) * k; };  // yaw grows to the left
    auto toY = [&](float pitch) { return oy - (pitch - midPitch) * k; };

    kit::HLine(plot.x, plot.Bottom(), plot.w, theme::kHairline);
    std::vector<float> xy;
    const size_t stride = std::max<size_t>(1, s.size() / 400);  // ~400 points: enough for the shape
    for (size_t i = 0; i < s.size(); i += stride) {
        xy.push_back(toX(skaar::AngleDelta(s[i].yaw, yaw0)));
        xy.push_back(toY(s[i].pitch));
    }
    chart::Polyline(xy, theme::kCream.Alpha(0.35f), 1);
    // where the view was at each kill (kill.t is from the file: bound the index it maps to)
    const float lastSample = static_cast<float>(s.size() - 1) / skaar::kSampleHz;
    std::vector<float> killDots;
    for (const skaar::KillEvent& kill : rec.kills) {
        const size_t i = std::min(s.size() - 1, static_cast<size_t>(RunTime(kill.t, lastSample) * skaar::kSampleHz));
        killDots.push_back(toX(skaar::AngleDelta(s[i].yaw, yaw0)));
        killDots.push_back(toY(s[i].pitch));
    }
    chart::Dots(killDots, 5, kEmber);
    Label(plot.x, plot.Bottom() + 24, skaar::Va("%.0f° × %.0f°", spanYaw, spanPitch), theme::kCreamFaint);

    // angular speed over the run, smoothed
    std::vector<float> speed;
    const int window = 6;
    for (size_t i = window; i < s.size(); i += 3) {
        const float dt = s[i].t - s[i - window].t;
        const float d = std::hypot(skaar::AngleDelta(s[i].yaw, s[i - window].yaw), s[i].pitch - s[i - window].pitch);
        speed.push_back(dt > 0 ? d / dt : 0);
    }
    if (speed.size() >= 2) {
        std::vector<float> sorted = speed;
        std::sort(sorted.begin(), sorted.end());
        const float p98 = std::max(10.0f, sorted[sorted.size() * 98 / 100]);
        const Rect sp{r.x + 60, plot.Bottom() + 64, r.w - 60, r.h - plot.h - 100};
        Label(r.x, sp.y - 14, "VIEW SPEED · °/S");
        float lo, hi, step;
        chart::NiceRange(0, p98, 2, lo, hi, step);
        chart::YAxis(sp, lo, hi, step, "%.0f");
        chart::Curve(sp, speed, lo, hi, theme::kCreamSoft, 1.0f);
    }
}

// The run a run is compared with: the pinned one (same scenario), else the
// scenario's best. Empty if there's nothing else to compare with.
std::string CompareFile(const RunSummary& s, const std::string& self, std::string* why) {
    if (!ui.pinnedRun.empty() && ui.pinnedRun != self) {
        const int i = IndexOfFile(ui.pinnedRun);
        if (i >= 0 && data.history[i].scenario == s.scenario) {
            *why = "PINNED, " + FormatDate(data.history[i].when, data.now);
            return ui.pinnedRun;
        }
    }
    const auto it = data.perScenario.find(s.scenario);
    if (it != data.perScenario.end() && it->second.bestRun >= 0) {
        const RunSummary& best = data.history[it->second.bestRun];
        if (best.file != self && !best.file.empty()) {
            *why = "BEST, " + FormatDate(best.when, data.now);
            return best.file;
        }
    }
    return "";
}

// "+2.1" style delta line under a cell value; better = brighter
void Delta(const Rect& r, float delta, const char* format, bool higherIsBetter) {
    if (std::fabs(delta) < 1e-6f) {
        Label(r.x + 22, r.y + 98, "SAME", theme::kCreamFaint);
        return;
    }
    const bool better = higherIsBetter ? delta > 0 : delta < 0;
    Label(r.x + 22, r.y + 98, skaar::Va(format, delta), better ? theme::kCreamSoft : theme::kCreamFaint);
}

void DrawRun(float top) {
    const kit::Fonts& f = kit::GetFonts();
    const float m = theme::kMargin, w = kit::Width() - 2 * m, h = kit::Height();
    const skaar::RunRecord* rec = LoadRunRecord(ui.selectedRun);
    if (!rec) {
        f.text.Draw(m, top + 60, type::kBody, ui.runError.empty() ? "No run selected." : ui.runError, theme::kCreamSoft);
        return;
    }
    // copy: loading the comparison may evict this record from the cache
    const skaar::RunRecord run = *rec;
    const RunSummary& s = run.summary;
    const bool hold = IsHold(s.scenario);
    const int index = IndexOfFile(ui.selectedRun);

    std::string compareWhy;
    const std::string compareFile = CompareFile(s, ui.selectedRun, &compareWhy);
    const skaar::RunRecord* other = compareFile.empty() ? nullptr : LoadRunRecord(compareFile);
    const RunSummary* o = other ? &other->summary : nullptr;

    Label(m, top + 10, "RUN   ·   " + FormatDate(s.when, data.now) + "   ·   " + ScenarioName(s) + "   ·   " +
                           FormatDuration(s.duration));
    const bool pb = index >= 0 && data.perScenario.count(s.scenario) &&
                    data.perScenario.at(s.scenario).bestRun == index;
    f.display.Draw(m - 6, top + 110, type::kBigNumber, skaar::Va("%.0f", s.score), pb ? kEmber : theme::kCream, -0.02f);
    if (index >= 0) {
        f.text.Draw(m, top + 150, 22, Verdict(index), theme::kCreamSoft);
    }

    // numbers, with the difference to the comparison run under each
    const float cx = m + w * 0.38f, cw = w - (cx - m), cellW = cw / 6, cellH = 124;
    auto cell = [&](int i) { return Rect{cx + i * cellW, top + 30, cellW, cellH}; };
    CellGrid(cx, top + 30, cellW, cellH, 6, 6);
    Cell(cell(0), hold ? "ON TARGET" : "ACCURACY", Pct(s.accuracy));
    Cell(cell(1), "KILLS", skaar::Va("%d", s.kills));
    Cell(cell(2), "MEAN TTK", s.ttk > 0 ? skaar::Va("%.0f MS", s.ttk) : "—");
    Cell(cell(3), hold ? "TIME ON" : "HITS", hold ? skaar::Va("%.1f S", s.onTarget) : skaar::Va("%d/%d", s.hits, s.shots));
    Cell(cell(4), "CM / 360", s.sensCm360 > 0 ? skaar::Va("%.1f", s.sensCm360) : "—");
    Cell(cell(5), "FOV", s.fov > 0 ? skaar::Va("%.0f", s.fov) : "—");
    if (o) {
        Delta(cell(0), (s.accuracy - o->accuracy) * 100, "%+.1f PT", true);
        Delta(cell(1), static_cast<float>(s.kills - o->kills), "%+.0f", true);
        if (s.ttk > 0 && o->ttk > 0) {
            Delta(cell(2), s.ttk - o->ttk, "%+.0f MS", false);
        }
        if (hold) {
            Delta(cell(3), s.onTarget - o->onTarget, "%+.1f S", true);
        } else {
            Delta(cell(3), static_cast<float>(s.hits - o->hits), "%+.0f", true);
        }
        if (std::fabs(s.sensCm360 - o->sensCm360) > 0.05f && o->sensCm360 > 0) {
            Label(cell(4).x + 22, cell(4).y + 98, skaar::Va("WAS %.1f", o->sensCm360), theme::kCreamFaint);
        }
        Label(cx, top + 30 + cellH + 26, "COMPARED WITH  " + compareWhy + skaar::Va("  ·  %.0f", o->score),
              theme::kCreamFaint);
    }

    // three panels
    const float py = top + 214, ph = h - py - 230, gap = 70;
    const float pw = (w - 2 * gap) / 3;
    DrawTtkPanel({m, py, pw, ph}, run, hold, other);
    DrawDispersionPanel({m + pw + gap, py, pw, ph}, run, hold);
    DrawPathPanel({m + 2 * (pw + gap), py, pw, ph}, run);

    const float by = h - 196;
    if (ActionButton({m, by, 330, 64}, "Replay", "IN GAME")) {
        Exec(skaar::Va("sk_replay %s\n", ui.selectedRun.c_str()));
        Close();
    }
    if (data.FindScenario(s.scenario) && ActionButton({m + 350, by, 330, 64}, "Play again", "")) {
        StartScenario(s.scenario);
    }
    const bool pinned = ui.pinnedRun == ui.selectedRun;
    if (ActionButton({m + 700, by, 330, 64}, pinned ? "Unpin" : "Pin to compare", "")) {
        ui.pinnedRun = pinned ? "" : ui.selectedRun;
    }
}

// ---- arena ------------------------------------------------------------------------------

void DrawArena(float top) {
    const kit::Fonts& f = kit::GetFonts();
    const float m = theme::kMargin, w = kit::Width() - 2 * m;
    const auto& sessions = data.arena;
    if (sessions.empty()) {
        f.display.Draw(m - 4, top + 90, type::kTitle, "NO ARENA SESSIONS YET.", theme::kCream, theme::kTrackStatement);
        Paragraph(m, top + 120, 760,
                  "Play the arena from the main menu. Each session's accuracy per weapon, damage and frags land here "
                  "when you leave the map.",
                  type::kBody, theme::kCreamSoft);
        return;
    }

    // totals and the latest session
    const skaar::ArenaSession& last = sessions.back();
    float seconds = 0;
    int kills = 0, deaths = 0;
    for (const auto& s : sessions) {
        seconds += s.seconds;
        kills += s.kills;
        deaths += s.deaths;
    }
    const float cellW = w / 5, cellH = 136;
    CellGrid(m, top, cellW, cellH, 5, 5);
    auto cell = [&](int i, const char* label, const std::string& value, const std::string& sub) {
        const Rect r{m + i * cellW, top, cellW, cellH};
        Cell(r, label, value, theme::kCream, 34);
        Label(r.x + 22, r.y + 116, sub, theme::kCreamFaint);
    };
    cell(0, "SESSIONS", skaar::Va("%zu", sessions.size()), FormatDuration(seconds) + " PLAYED");
    cell(1, "LAST SESSION", FormatDuration(last.seconds), FormatDate(last.when, data.now));
    cell(2, "DAMAGE", skaar::Va("%d", last.damageDealt), skaar::Va("TAKEN %d", last.damageTaken));
    cell(3, "FRAGS", skaar::Va("%d", last.kills), skaar::Va("DEATHS %d", last.deaths));
    cell(4, "ALL FRAGS", skaar::Va("%d", kills), deaths ? skaar::Va("RATIO %.2f", static_cast<float>(kills) / deaths)
                                                         : std::string("NO DEATHS"));

    // accuracy per weapon over sessions: one small chart each
    std::vector<std::string> ids;
    for (const skaar::WeaponDef& wd : skaar::Weapons()) {
        for (const auto& s : sessions) {
            if (s.weapons.count(wd.id)) {
                ids.push_back(wd.id);
                break;
            }
        }
    }
    const float chartTop = top + cellH + 90;
    Label(m, chartTop - 40, "ACCURACY PER SESSION");
    const int n = static_cast<int>(ids.size());
    const float gap = 70, cw = n ? (w - gap * (n - 1)) / n : w;
    for (int i = 0; i < n; i++) {
        const skaar::WeaponDef* wd = skaar::WeaponById(ids[i]);
        std::vector<float> acc;
        float bestAcc = 0;
        for (const auto& s : sessions) {
            auto it = s.weapons.find(ids[i]);
            if (it != s.weapons.end() && it->second.shots > 0) {
                acc.push_back(it->second.Accuracy() * 100);
                bestAcc = std::max(bestAcc, it->second.Accuracy() * 100);
            }
        }
        const float x = m + i * (cw + gap);
        f.text.Draw(x, chartTop, 26, wd ? wd->name : ids[i], theme::kCream);
        const float latest = acc.empty() ? 0 : acc.back();
        f.mono.Draw(x + cw, chartTop, 26, skaar::Va("%.0f%%", latest), theme::kCream, 0.01f, Align::kRight);
        Label(x + cw, chartTop + 28, skaar::Va("BEST %.0f%%", bestAcc), theme::kCreamFaint, Align::kRight);
        const Rect plot{x + 50, chartTop + 60, cw - 50, 220};
        if (acc.size() >= 2) {
            float lo, hi, step;
            const auto [mn, mx] = std::minmax_element(acc.begin(), acc.end());
            chart::NiceRange(std::max(0.0f, *mn - 5), std::min(100.0f, *mx + 5), 3, lo, hi, step);
            chart::YAxis(plot, lo, hi, step, "%.0f");
            chart::Curve(plot, acc, lo, hi, theme::kCreamSoft, 1.5f, theme::kCream.Alpha(0.035f));
        } else {
            f.text.Draw(x, plot.y + 40, 20, "Needs two sessions.", theme::kCreamFaint);
        }
    }
}

// ---- benchmarks ------------------------------------------------------------------------

void DrawBenchmarks(float top) {
    const kit::Fonts& f = kit::GetFonts();
    const float m = theme::kMargin, w = kit::Width() - 2 * m;
    if (data.progress.empty()) {
        f.text.Draw(m, top + 60, type::kBody, "No benchmarks found. Add one to benchmarks/ (docs/scenarios.md).",
                    theme::kCreamSoft);
        return;
    }
    float y = top;
    for (const insights::BenchmarkProgress& p : data.progress) {
        const skaar::Benchmark& b = *p.benchmark;
        const int tiers = static_cast<int>(b.tiers.size());
        f.display.Draw(m - 4, y + 60, 56, kit::Upper(b.name), theme::kCream, theme::kTrackStatement);
        f.text.Draw(m, y + 100, 22, b.description, theme::kCreamSoft);
        Label(m + w, y + 30, "STANDING", theme::kCreamFaint, Align::kRight);
        f.display.Draw(m + w, y + 82, 44, kit::Upper(p.overallTier >= 0 ? b.tiers[p.overallTier] : "Unranked"),
                       theme::kCream, theme::kTrackStatement, Align::kRight);

        // columns
        const float colBest = m + 470, colTier = m + 560, colBar = m + 780, barW = w - 780 - 220, colNext = m + w;
        y += 160;
        Label(m, y, "SCENARIO");
        Label(colBest, y, "BEST", theme::kCreamFaint, Align::kRight);
        Label(colTier, y, "TIER");
        // tier ladder above the bars
        for (int t = 0; t < tiers; t++) {
            Label(colBar + barW * (t + 1) / tiers, y, b.tiers[t], theme::kCreamFaint, Align::kRight);
        }
        Label(colNext, y, "NEXT AT", theme::kCreamFaint, Align::kRight);
        y += 20;
        kit::HLine(m, y, w, theme::kHairline);

        const float rowH = 60;
        for (size_t i = 0; i < p.entries.size(); i++) {
            const insights::BenchmarkEntryProgress& e = p.entries[i];
            const Rect r{m - 40, y, w + 40, rowH};
            const int id = Focusable(r);
            const bool focused = Focused(id);
            const bool recommended = static_cast<int>(i) == p.recommended;
            const float baseline = y + rowH / 2 + 9;
            if (focused) {
                Marker(m - 28, baseline, 18);
            }
            const Color c = focused ? theme::kCream : theme::kCreamSoft;
            f.text.Draw(m, baseline, 24, data.NameOf(e.scenario), c);
            if (recommended) {
                Label(m + f.text.Measure(24, data.NameOf(e.scenario)) + 24, baseline - 2, "FOCUS NEXT", kEmber);
            }
            f.mono.Draw(colBest, baseline, 22, e.played ? skaar::Va("%.0f", e.best) : "—", c, 0.01f, Align::kRight);
            Label(colTier, baseline - 2, e.tier >= 0 ? b.tiers[e.tier] : (e.played ? "Below " + b.tiers[0] : "Unplayed"),
                  e.tier >= 0 ? theme::kCream : theme::kCreamFaint);
            // overall position on the ladder: tier + progress, out of `tiers`
            const float pos = e.played ? std::min<float>(tiers, (e.tier + 1) + (e.tier + 1 < tiers ? e.progress : 0)) : 0;
            chart::Progress({colBar, baseline - 8, barW, 3}, pos / tiers, theme::kHairlineStrong,
                            focused ? theme::kCream : theme::kCreamSoft);
            for (int t = 1; t < tiers; t++) {
                kit::VLine(colBar + barW * t / tiers, baseline - 14, 15, theme::kHairlineStrong);
            }
            const auto& thresholds = b.entries[i].thresholds;
            const std::string next = e.tier + 1 < tiers ? skaar::Va("%.0f", thresholds[e.tier + 1]) : "MAX";
            f.mono.Draw(colNext, baseline, 22, next, theme::kCreamFaint, 0.01f, Align::kRight);
            kit::HLine(m, y + rowH, w, theme::kHairline);
            if (Activated(id, r)) {
                StartScenario(e.scenario);
                return;
            }
            y += rowH;
        }
        if (!p.weakestCategory.empty()) {
            Label(m, y + 44, "WEAKEST AREA  ·  " + p.weakestCategory + "     ENTER  PLAYS THE SCENARIO", theme::kCreamFaint);
        }
        y += 100;
        Label(m + w, y - 56, "THRESHOLDS ARE FIRST GUESSES, NOT YET CALIBRATED", theme::kCreamFaint, Align::kRight);
    }
}

}  // namespace

// Score per run with a nice axis; each run is a dot (PB in ember). Hovering a
// dot shows its score and date; clicking opens the run.
void ScoreChart(const Rect& r, const std::vector<int>& runs, bool drawAxis) {
    if (runs.empty()) {
        return;
    }
    const size_t from = runs.size() > 60 ? runs.size() - 60 : 0;
    std::vector<float> scores;
    int best = -1;
    for (size_t k = from; k < runs.size(); k++) {
        scores.push_back(data.history[runs[k]].score);
        if (best < 0 || data.history[runs[k]].score > data.history[runs[best]].score) {
            best = static_cast<int>(k);
        }
    }
    const auto [mn, mx] = std::minmax_element(scores.begin(), scores.end());
    float lo, hi, step;
    chart::NiceRange(*mn * 0.97f, *mx * 1.01f, 4, lo, hi, step);
    if (drawAxis) {
        chart::YAxis(r, lo, hi, step, "compact");
    }
    chart::Curve(r, scores, lo, hi, theme::kCreamSoft, 1.5f, theme::kCream.Alpha(0.035f));

    const size_t n = scores.size();
    int hover = -1;
    for (size_t i = 0; i < n; i++) {
        const float x = n == 1 ? r.x : r.x + r.w * i / (n - 1);
        const float y = r.Bottom() - (scores[i] - lo) / (hi - lo) * r.h;
        const bool isBest = static_cast<int>(i + from) == best;
        chart::Disc(x, y, isBest ? 6 : 3.5f, isBest ? kEmber : theme::kCream);
        if (in.mouseUsed && std::hypot(in.mx - x, in.my - y) < 14) {
            hover = static_cast<int>(i);
        }
    }
    if (hover >= 0) {
        const RunSummary& run = data.history[runs[hover + from]];
        const float x = n == 1 ? r.x : r.x + r.w * hover / (n - 1);
        const float y = r.Bottom() - (scores[hover] - lo) / (hi - lo) * r.h;
        chart::Ring(x, y, 10, theme::kCream, 1.5f);
        const std::string tip = skaar::Va("%.0f   ·   %s", run.score, FormatDate(run.when, data.now).c_str());
        const float tw = kit::GetFonts().mono.Measure(type::kLabel, kit::Upper(tip), theme::kTrackMono) + 32;
        const float tx = std::clamp(x - tw / 2, r.x - 60, r.Right() - tw);
        kit::Fill(tx, y - 62, tw, 40, theme::kStage);
        kit::HLine(tx, y - 62, tw, theme::kHairlineStrong);
        kit::HLine(tx, y - 22, tw, theme::kHairlineStrong);
        Label(tx + 16, y - 36, tip, theme::kCream);
        if (in.click && !run.file.empty()) {
            in.click = false;
            ShowRun(run.file);
        }
    }
}

void DrawInsights() {
    const float m = theme::kMargin, w = kit::Width(), h = kit::Height();
    if (InGame()) {
        kit::Fill(0, 0, w, h, theme::kStage.Alpha(0.95f));
    } else {
        kit::Background(true);
    }
    Header("");

    struct T {
        const char* label;
        InsightsTab tab;
    };
    std::vector<T> tabs = {{"Overview", InsightsTab::kOverview},
                           {"Scenarios", InsightsTab::kScenarios},
                           {"Benchmarks", InsightsTab::kBenchmarks}};
    tabs.push_back({"Arena", InsightsTab::kArena});
    if (!ui.selectedRun.empty()) {
        tabs.push_back({"Run", InsightsTab::kRun});
    }
    float x = m;
    for (const T& t : tabs) {
        float tw = 0;
        if (Tab(x, 186, t.label, ui.insightsTab == t.tab, &tw)) {
            ui.insightsTab = t.tab;
        }
        x += tw + 44;
    }

    const float top = 232;
    switch (ui.insightsTab) {
        case InsightsTab::kOverview:
            DrawOverview(top);
            break;
        case InsightsTab::kScenarios:
            DrawScenarios(top);
            break;
        case InsightsTab::kRun:
            DrawRun(top);
            break;
        case InsightsTab::kBenchmarks:
            DrawBenchmarks(top);
            break;
        case InsightsTab::kArena:
            DrawArena(top);
            break;
    }
    // Left/Right nobody used: next/previous tab
    if (const int dir = TakeAdjust()) {
        int i = 0;
        for (size_t k = 0; k < tabs.size(); k++) {
            if (tabs[k].tab == ui.insightsTab) {
                i = static_cast<int>(k);
            }
        }
        const int n = static_cast<int>(tabs.size());
        ui.insightsTab = tabs[(i + dir + n) % n].tab;
        ui.focus = 0;
        ResetMarker();
    }
    // problems with the data on disk, said plainly
    if (data.historySkipped) {
        Label(m, h - 96, skaar::Va("%d LINE(S) IN STATS/HISTORY.JSONL COULDN'T BE READ AND ARE LEFT OUT (THE FILE IS UNCHANGED)",
                                   data.historySkipped),
              kEmber);
    }
    if (!data.benchmarkErrors.empty() && ui.insightsTab == InsightsTab::kBenchmarks) {
        const auto& [id, error] = data.benchmarkErrors.front();
        Label(m, h - 120, skaar::Va("BENCHMARKS/%s.JSON NOT LOADED  ·  %s", id.c_str(), error.c_str()), kEmber);
    }
    Footer("← →  TABS     ESC  BACK");
}

}  // namespace ui
