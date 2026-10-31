// Aim trainer glue: scenario lifecycle, per-command hit registration, targets,
// HUD, end-of-run summary, run records and replays. The simulation itself is
// engine-free (game/shared/trainer_sim.*).
//
// This file is part of Skaar. GPLv2 or later.
#include "trainer.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "../shared/charts.h"
#include "../shared/draw2d.h"
#include "../shared/insights.h"
#include "../shared/move_ruleset.h"
#include "../shared/reticle.h"
#include "../shared/runlog.h"
#include "../shared/sens.h"
#include "../shared/scenario.h"
#include "../shared/trainer_sim.h"
#include "local.h"
#include "weapons.h"

namespace cg::trainer {
namespace {

namespace kit = skaar::ui;
namespace theme = skaar::ui::theme;
namespace chart = skaar::ui::chart;
using kit::Align;
using kit::Color;
using skaar::RunRecord;
using skaar::RunSummary;
using skaar::Scenario;
using skaar::TrainerSim;

constexpr int kCountdownMs = 3000;
constexpr float kHitFlash = 0.07f;    // seconds a hit target flashes
constexpr int kKillFlashMs = 90;       // crosshair flash on a kill
constexpr Color kTarget = theme::kEmber;  // pastel red, the accent: targets are the object
constexpr Color kTargetHit = skaar::ui::Rgb(252, 236, 234);  // a hit blanches the target for a moment

enum class Phase {
    kOff,        // no scenario: free roam
    kWaitSpawn,  // scenario loaded, waiting for the first command to anchor it
    kCountdown,
    kRunning,
    kSummary,
    kReplay,
};

struct Cvars {
    vmCvar_t scenario, dpi, dpiLegacy, sensitivity, mYaw;
    vmCvar_t chStyle, chSize, chGap, chThickness, chColor, chOutline, hitSound, volumeHint;
};

struct State {
    Phase phase = Phase::kOff;
    Scenario scenario;
    bool scenarioLoaded = false;
    std::string loadError;

    TrainerSim sim;
    skaar::Vec3 anchor;
    float baseYaw = 0;
    bool waitingSpawn = false;  // a respawn was sent: anchor on the command that sees it land
    int spawnEFlags = 0;        // ps.eFlags when it was sent; the server's Spawn() toggles EF_TELEPORT_BIT
    uint32_t seed = 1;

    int countdownEnd = 0;     // cg.time the run starts
    int runStart = 0;         // command serverTime of t = 0
    int lastCmdTime = 0;
    bool attackWasDown = false;
    float nextSample = 0;
    std::vector<skaar::ViewSample> samples;
    int killFlashUntil = 0;

    // previous runs of this scenario (for PB / average) and the run just saved
    std::vector<RunSummary> history;
    std::optional<RunRecord> last;
    bool lastIsPb = false;
    float previousBest = 0;
    std::string lastFile;

    // replay
    RunRecord replay;
    int replayStart = 0;
    size_t replayKill = 0, replayShot = 0;

    qhandle_t model = 0, shader = 0;
    sfxHandle_t hitSound = 0, killSound = 0, gunSound = 0;
} st;

Cvars cv;

// ---- helpers -------------------------------------------------------------------

void RegisterCvars() {
    trap::CvarRegister(&cv.scenario, "sk_scenario", "", CVAR_ARCHIVE);
    // sens_dpi is the cvar docs/feel.md documents and sens_test divides by; the aim
    // screens read it too. Its default is 0 = unset, so the record falls back to 800.
    trap::CvarRegister(&cv.dpi, "sens_dpi", "0", CVAR_ARCHIVE);
    // sk_dpi is what the aim screens wrote before they moved to sens_dpi (game/ui);
    // read it as a fallback so a record is right whichever order the two land in.
    // Not archived: it only mirrors a value the UI owns.
    trap::CvarRegister(&cv.dpiLegacy, "sk_dpi", "0", 0);
    trap::CvarRegister(&cv.sensitivity, "sensitivity", "2", CVAR_ARCHIVE);
    trap::CvarRegister(&cv.mYaw, "m_yaw", "0.022", CVAR_ARCHIVE);
    namespace rd = skaar::ui::reticle_defaults;
    trap::CvarRegister(&cv.chStyle, "cg_crosshairStyle", rd::kStyle, CVAR_ARCHIVE);
    trap::CvarRegister(&cv.chSize, "cg_crosshairSize", rd::kSize, CVAR_ARCHIVE);
    trap::CvarRegister(&cv.chGap, "cg_crosshairGap", rd::kGap, CVAR_ARCHIVE);
    trap::CvarRegister(&cv.chThickness, "cg_crosshairThickness", rd::kThickness, CVAR_ARCHIVE);
    trap::CvarRegister(&cv.chColor, "cg_crosshairColor", rd::kColor, CVAR_ARCHIVE);
    trap::CvarRegister(&cv.chOutline, "cg_crosshairOutline", rd::kOutline, CVAR_ARCHIVE);
    trap::CvarRegister(&cv.hitSound, "sk_hitSound", "1", CVAR_ARCHIVE);
}

void UpdateCvars() {
    for (vmCvar_t* c : {&cv.scenario, &cv.dpi, &cv.dpiLegacy, &cv.sensitivity, &cv.mYaw, &cv.chStyle, &cv.chSize, &cv.chGap,
                        &cv.chThickness, &cv.chColor, &cv.chOutline, &cv.hitSound}) {
        trap::CvarUpdate(c);
    }
}

float CmPer360() {
    // 0 = unset everywhere: sens_dpi first (documented), then the screens' older
    // sk_dpi, then the 800 the aim screens also assume when nothing is set
    float dpi = cv.dpi.value;
    if (!(dpi > 0)) {
        dpi = cv.dpiLegacy.value;
    }
    if (!(dpi > 0)) {
        dpi = 800;
    }
    return skaar::sens::CmPer360(cv.sensitivity.value, cv.mYaw.value, dpi);
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

uint32_t NewSeed() {
    const skaar::RunTime n = Now();
    return static_cast<uint32_t>(n.Seconds() * 2654435761u) ^ static_cast<uint32_t>(trap::Milliseconds() * 40503u);
}

bool UiOpen() { return trap::Key_GetCatcher() & KEYCATCH_UI; }

void LoadHistory() {
    st.history.clear();
    std::string text;
    if (trap::FileRead(skaar::kHistoryPath, text)) {
        for (RunSummary& r : skaar::ParseHistory(text)) {
            if (r.scenario == st.scenario.id) {
                st.history.push_back(std::move(r));
            }
        }
    }
}

float BestScore() {
    float best = 0;
    for (const RunSummary& r : st.history) {
        best = std::max(best, r.score);
    }
    return best;
}

bool LoadScenario(const std::string& id) {
    st.scenarioLoaded = false;
    st.loadError.clear();
    if (id.empty()) {
        return false;
    }
    std::string text;
    if (!trap::FileRead(skaar::Va("scenarios/%s.json", id.c_str()), text)) {
        st.loadError = "scenario '" + id + "' not found";
        Com_Printf(S_COLOR_YELLOW "WARNING: trainer: %s\n", st.loadError.c_str());
        return false;
    }
    std::string error;
    if (!skaar::ParseScenario(id, text, st.scenario, error)) {
        st.loadError = id + ": " + error;
        Com_Printf(S_COLOR_YELLOW "WARNING: trainer: %s\n", st.loadError.c_str());
        return false;
    }
    st.scenarioLoaded = true;
    LoadHistory();
    return true;
}

void SetFreeze(bool frozen) { trap::SendClientCommand(frozen ? "sk_freeze 1" : "sk_freeze 0"); }

// New run: fresh seed (unless the scenario fixes one), targets placed, countdown.
void BeginRun() {
    st.gunSound = st.scenario.weapon.empty()
                      ? 0
                      : trap::S_RegisterSound(skaar::Va("sound/weapons/%s.wav", st.scenario.weapon.c_str()), false);
    st.seed = st.scenario.seed ? st.scenario.seed : NewSeed();
    st.sim.Start(st.scenario, st.seed, st.anchor, st.baseYaw);
    st.samples.clear();
    st.nextSample = 0;
    st.attackWasDown = true;  // a click held from the menu doesn't count as a shot
    st.countdownEnd = cg.time + kCountdownMs;
    st.phase = Phase::kCountdown;
    SetFreeze(!st.scenario.playerMove);
    Com_DPrintf("trainer: run of %s, seed %u\n", st.scenario.id.c_str(), st.seed);
}

// Start (or restart) the loaded scenario: back to the spawn, then count down.
void Restart() {
    if (!st.scenarioLoaded) {
        trap::SendClientCommand("respawn");
        return;
    }
    if (st.scenario.map != cg.mapname) {
        trap::SendConsoleCommand(skaar::Va("map %s\n", st.scenario.map.c_str()));
        return;
    }
    // One respawn at a time: Spawn() only toggles EF_TELEPORT_BIT, so two respawns in
    // flight flip it twice and the anchor never sees one land (sk_restart;sk_restart on
    // one line). The restart already in flight does what this one would ask for.
    if (st.waitingSpawn) {
        return;
    }
    // Always take the anchor from the spawn the server actually gives us: a
    // restart can happen with the player frozen anywhere on the map, and on a
    // map with several spawn points it is not the one we last anchored at.
    // The respawn travels as a client command, so the anchor is not known yet
    // (see OnCommand).
    st.waitingSpawn = cg.snap != nullptr;  // before the first snapshot there is no respawn to wait for
    if (cg.snap) {
        st.spawnEFlags = cg.snap->ps.eFlags;
    }
    st.phase = Phase::kWaitSpawn;
    trap::SendClientCommand("respawn");
}

void Stop() {
    st.phase = Phase::kOff;
    SetFreeze(false);
}

void SaveRun() {
    RunRecord record = st.sim.Record();
    RunSummary& s = record.summary;
    s.when = Now();
    s.sensCm360 = CmPer360();
    s.fov = cvars.fov.value;
    s.file = skaar::RecordPath(s);
    record.samples = std::move(st.samples);
    st.samples.clear();

    st.previousBest = BestScore();
    st.lastIsPb = st.history.empty() || s.score > st.previousBest;

    const bool wroteRecord = trap::FileWrite(s.file.c_str(), skaar::RecordToJson(record), false);
    if (!wroteRecord) {
        Com_Printf(S_COLOR_YELLOW "WARNING: trainer: could not write %s\n", s.file.c_str());
        s.file.clear();
    }
    if (!trap::FileWrite(skaar::kHistoryPath, skaar::SummaryToJsonLine(s) + "\n", true)) {
        Com_Printf(S_COLOR_YELLOW "WARNING: trainer: could not append %s\n", skaar::kHistoryPath);
    }
    Com_Printf("trainer: %s  score %.0f  accuracy %.1f%%  kills %d  ttk %.0f ms%s\n", s.name.c_str(), s.score,
               s.accuracy * 100, s.kills, s.ttk, st.lastIsPb ? "  (personal best)" : "");
    st.lastFile = s.file;
    st.history.push_back(s);
    st.last = std::move(record);  // s refers into record: last use above
}

// ---- replay ----------------------------------------------------------------------

bool StartReplay(const std::string& file) {
    std::string path = file;
    if (path.find('/') == std::string::npos) {
        path = std::string(skaar::kRunsDir) + "/" + path;
    }
    if (path.size() < 5 || path.compare(path.size() - 5, 5, ".json") != 0) {
        path += ".json";
    }
    std::string text, error;
    if (!trap::FileRead(path.c_str(), text)) {
        Com_Printf(S_COLOR_YELLOW "WARNING: trainer: no run record %s\n", path.c_str());
        return false;
    }
    RunRecord record;
    if (!skaar::RecordFromJson(text, record, error)) {
        Com_Printf(S_COLOR_YELLOW "WARNING: trainer: bad run record %s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    if (record.samples.empty()) {
        Com_Printf(S_COLOR_YELLOW "WARNING: trainer: %s has no view samples to replay\n", path.c_str());
        return false;
    }
    if (!LoadScenario(record.summary.scenario)) {
        return false;
    }
    if (st.scenario.map != cg.mapname) {
        // load the map first; the replay starts once it has
        trap::CvarSet("sk_scenario", st.scenario.id.c_str());
        trap::CvarSet("sk_pendingReplay", path.c_str());
        trap::SendConsoleCommand(skaar::Va("map %s\n", st.scenario.map.c_str()));
        return true;
    }

    // targets are anchored where the recorded run's first sample was
    const skaar::ViewSample& first = record.samples.front();
    st.replay = std::move(record);
    st.anchor = {first.x, first.y, first.z};
    st.sim.Start(st.scenario, st.replay.seed, st.anchor, st.baseYaw);
    st.replayStart = cg.time;
    st.replayKill = st.replayShot = 0;
    st.phase = Phase::kReplay;
    SetFreeze(true);
    Com_Printf("trainer: replaying %s\n", path.c_str());
    return true;
}

// How far into the recorded run the replay is, in seconds.
float ReplayTime() { return (cg.time - st.replayStart) / 1000.0f; }

// The recorded camera at time t: the view samples around it, interpolated.
// Read from OverrideView rather than stored by AdvanceReplay: the view for a
// frame is built before AddEntities runs, so the camera has to be computed
// where it is needed (see OverrideView).
void ReplayCamera(float t, vec3_t origin, vec3_t angles) {
    const auto& s = st.replay.samples;
    auto it = std::lower_bound(s.begin(), s.end(), t, [](const skaar::ViewSample& v, float x) { return v.t < x; });
    const skaar::ViewSample& b = it == s.end() ? s.back() : *it;
    const skaar::ViewSample& a = it == s.begin() ? b : *(it - 1);
    const float k = b.t > a.t ? std::clamp((t - a.t) / (b.t - a.t), 0.0f, 1.0f) : 0;
    angles[YAW] = a.yaw + skaar::AngleDelta(b.yaw, a.yaw) * k;
    angles[PITCH] = -(a.pitch + (b.pitch - a.pitch) * k);
    angles[ROLL] = 0;
    origin[0] = a.x + (b.x - a.x) * k;
    origin[1] = a.y + (b.y - a.y) * k;
    origin[2] = a.z + (b.z - a.z) * k;
}

void AdvanceReplay() {
    const float t = ReplayTime();
    const auto& kills = st.replay.kills;
    const auto& shots = st.replay.shots;
    // apply recorded events in time order, so spawns draw from the RNG as they did
    while (st.replayKill < kills.size() && kills[st.replayKill].t <= t) {
        while (st.replayShot < shots.size() && shots[st.replayShot].t <= kills[st.replayKill].t) {
            if (shots[st.replayShot].hit) {
                st.sim.FlashSerial(shots[st.replayShot].target, shots[st.replayShot].t);
            }
            st.replayShot++;
        }
        st.sim.KillSerial(kills[st.replayKill].target, kills[st.replayKill].t);
        st.replayKill++;
    }
    while (st.replayShot < shots.size() && shots[st.replayShot].t <= t) {
        if (shots[st.replayShot].hit) {
            st.sim.FlashSerial(shots[st.replayShot].target, shots[st.replayShot].t);
        }
        st.replayShot++;
    }
    st.sim.Advance(t);

    if (t > st.replay.summary.duration + 1.5f) {
        st.phase = Phase::kOff;
        SetFreeze(false);
        trap::SendConsoleCommand(skaar::Va("skaar_ui run %s\n", st.replay.summary.file.c_str()));
    }
}

// ---- drawing ---------------------------------------------------------------------

void DrawCrosshair() {
    // the shared reticle (game/shared/reticle.h); kills flash it in the accent
    skaar::ui::Reticle reticle = skaar::ui::ReticleFromCvars(trap::CvarString);
    if (cg.time < st.killFlashUntil) {
        reticle.color = kTarget;
    }
    float cx, cy;
    skaar::ui::ScreenCentre(&cx, &cy);
    skaar::ui::DrawReticle(cx, cy, reticle);
}

std::string Clock(float seconds) {
    const int s = static_cast<int>(std::ceil(std::max(0.0f, seconds)));
    return skaar::Va("%d:%02d", s / 60, s % 60);
}

// Live numbers for a replay, from the recorded shots up to now (the replay sim
// itself fires no shots, so its own accuracy would read 0)
void ReplayNumbers(float* score, float* accuracy) {
    const auto& shots = st.replay.shots;
    int n = 0, hits = 0;
    for (size_t i = 0; i < st.replayShot && i < shots.size(); i++) {
        n++;
        hits += shots[i].hit ? 1 : 0;
    }
    *accuracy = n > 0 ? static_cast<float>(hits) / n : 0;
    const auto& sc = st.scenario.score;
    // as the summary and the sim itself: a hold scenario with a weapon fires like a
    // click run, and only a bare hold target has time on target to score
    if (st.scenario.fire == skaar::FireMode::kHold && !st.sim.HasWeapon()) {
        // aim samples are 20 Hz while firing: each hit sample is ~0.05 s on target
        *score = hits * 0.05f * sc.perSecondOnTarget + st.sim.Kills() * sc.kill;
    } else {
        *score = st.sim.Kills() * sc.kill * ((1 - sc.accuracyWeight) + sc.accuracyWeight * *accuracy);
    }
}

void DrawRunHud(bool replay) {
    // One quiet stack at the top centre: the score, then time and accuracy.
    // No labels; the numbers read for themselves.
    const kit::Fonts& f = kit::GetFonts();
    const float w = kit::Width();
    const TrainerSim& sim = st.sim;

    float score = sim.Score(), accuracy = sim.Accuracy();
    if (replay) {
        ReplayNumbers(&score, &accuracy);
    }
    f.display.Draw(w / 2, 96, 52, skaar::Va("%.0f", score), theme::kCream.Alpha(0.86f), -0.01f, Align::kCenter);
    std::string line = Clock(sim.Remaining()) + skaar::Va("   %.1f%%", accuracy * 100);
    if (replay) {
        line = "Replay   " + line;
    }
    f.text.Draw(w / 2, 130, 20, line, theme::kCreamSoft, 0.02f, Align::kCenter);
    if (replay) {
        f.text.Draw(w / 2, kit::Height() - 56, 18, "Esc for the menu", theme::kCreamFaint, 0.02f, Align::kCenter);
    }
}

void DrawCountdown() {
    const kit::Fonts& f = kit::GetFonts();
    const float w = kit::Width(), h = kit::Height();
    const int left = st.countdownEnd - cg.time;
    const int n = std::max(1, (left + 999) / 1000);
    const float phase = (left % 1000) / 1000.0f;  // 1 -> 0 within each second

    f.display.Draw(w / 2, h * 0.30f + 40, 64, st.scenario.name, theme::kCream, -0.01f, Align::kCenter);
    std::string meta = Clock(st.scenario.duration);
    if (!st.scenario.description.empty()) {
        meta = st.scenario.description + "   " + meta;
    }
    f.text.Draw(w / 2, h * 0.30f + 88, 22, meta, theme::kCreamSoft, 0, Align::kCenter);
    f.display.Draw(w / 2, h * 0.70f, 96, skaar::Va("%d", n), theme::kCream.Alpha(0.25f + 0.6f * phase), 0,
                   Align::kCenter);
    const float best = BestScore();
    if (best > 0) {
        f.text.Draw(w / 2, h * 0.70f + 48, 20, skaar::Va("Best %.0f", best), theme::kCreamFaint, 0.02f, Align::kCenter);
    }
}

void DrawSummary() {
    if (!st.last) {
        return;
    }
    const kit::Fonts& f = kit::GetFonts();
    const RunSummary& s = st.last->summary;
    const float w = kit::Width(), h = kit::Height(), m = theme::kMargin;
    // The sim only tracks time on target for a hold scenario with no weapon: one
    // with a weapon fires like a click scenario, so it must show the click cells.
    const bool hold = st.scenario.fire == skaar::FireMode::kHold && !st.sim.HasWeapon();

    kit::Fill(0, 0, w, h, theme::kStage.Alpha(0.86f));

    f.mono.Draw(m, 200, theme::kMonoSize, "RUN COMPLETE  ·  " + kit::Upper(s.name) + "  ·  " + Clock(s.duration),
                theme::kCreamSoft, theme::kTrackMono);
    f.display.Draw(m - 8, 340, 150, skaar::Va("%.0f", s.score), theme::kCream, -0.02f);

    std::string verdict;
    Color verdictColor = theme::kCreamSoft;
    if (st.lastIsPb && st.previousBest > 0) {
        verdict = skaar::Va("New personal best, up %.0f.", s.score - st.previousBest);
        verdictColor = kTarget;
    } else if (st.lastIsPb) {
        verdict = "First run. That's the score to beat.";
    } else {
        verdict = skaar::Va("%.0f below your best of %.0f.", st.previousBest - s.score, st.previousBest);
    }
    f.text.Draw(m, 400, 30, verdict, verdictColor);

    // stat cells sharing hairlines
    struct Cell {
        const char* label;
        std::string value;
    };
    const Cell cells[] = {
        {hold ? "ON TARGET" : "ACCURACY", skaar::Va("%.1f%%", s.accuracy * 100)},
        {"KILLS", skaar::Va("%d", s.kills)},
        {"MEAN TTK", s.kills ? skaar::Va("%.0f MS", s.ttk) : std::string("—")},
        {hold ? "TIME ON TARGET" : "SHOTS", hold ? skaar::Va("%.1f S", s.onTarget) : skaar::Va("%d / %d", s.hits, s.shots)},
    };
    const float cellW = 250, cellH = 112, top = 470;
    kit::HLine(m, top, cellW * 4, theme::kHairline);
    kit::HLine(m, top + cellH, cellW * 4, theme::kHairline);
    for (int i = 0; i < 4; i++) {
        const float x = m + i * cellW;
        kit::VLine(x, top, cellH, theme::kHairline);
        f.mono.Draw(x + 22, top + 36, 14, cells[i].label, theme::kCreamFaint, theme::kTrackMono);
        f.mono.Draw(x + 22, top + 84, 34, cells[i].value, theme::kCream, 0.01f);
    }
    kit::VLine(m + cellW * 4, top, cellH, theme::kHairline);

    // score history of this scenario, this run in ember
    const chart::Rect plot{m + 60, 680, cellW * 4 - 60, 170};
    std::vector<float> scores;
    const size_t from = st.history.size() > 30 ? st.history.size() - 30 : 0;
    for (size_t i = from; i < st.history.size(); i++) {
        scores.push_back(st.history[i].score);
    }
    f.mono.Draw(m, 650, 14, skaar::Va("LAST %zu RUNS", scores.size()), theme::kCreamFaint, theme::kTrackMono);
    if (scores.size() >= 2) {
        float lo, hi, step;
        const auto [mn, mx] = std::minmax_element(scores.begin(), scores.end());
        chart::NiceRange(*mn * 0.95f, *mx * 1.02f, 3, lo, hi, step);
        chart::YAxis(plot, lo, hi, step, "compact");
        chart::Curve(plot, scores, lo, hi, theme::kCreamSoft, 1.5f, theme::kCream.Alpha(0.04f));
        const float y = plot.Bottom() - (scores.back() - lo) / (hi - lo) * plot.h;
        chart::Disc(plot.Right(), y, 6, kTarget);
    }

    // shot dispersion: where shots landed around the target, in target radii
    const float dcx = w - m - 230, dcy = 560, dr = 200;
    f.mono.Draw(w - m - 460, 330, 14, hold ? "AIM ERROR WHILE FIRING" : "SHOT DISPERSION", theme::kCreamFaint,
                theme::kTrackMono);
    chart::Ring(dcx, dcy, dr / 2.5f, theme::kCreamFaint, 1);
    chart::Ring(dcx, dcy, dr, theme::kHairlineStrong, 1);
    kit::HLine(dcx - dr, dcy, dr * 2, theme::kHairline);
    kit::VLine(dcx, dcy - dr, dr * 2, theme::kHairline);
    float sx = 0, sy = 0;
    int n = 0;
    // all shots count toward the drift; at most ~1200 are drawn (the render queue is finite)
    const auto& shots = st.last->shots;
    const size_t stride = std::max<size_t>(1, shots.size() / 1200);
    std::vector<float> hitDots, missDots;
    for (size_t i = 0; i < shots.size(); i++) {
        const skaar::ShotEvent& shot = shots[i];
        if (shot.radius <= 0) {
            continue;
        }
        const float ux = shot.dx / shot.radius, uy = shot.dy / shot.radius;
        sx += ux;
        sy += uy;
        n++;
        if (i % stride || std::fabs(ux) > 2.5f || std::fabs(uy) > 2.5f) {
            continue;  // thinned, or off the plot (2.5 radii fill it)
        }
        std::vector<float>& dots = shot.hit ? hitDots : missDots;
        dots.push_back(dcx + ux / 2.5f * dr);
        dots.push_back(dcy - uy / 2.5f * dr);
    }
    chart::Dots(missDots, 5, kTarget.Alpha(0.8f));
    chart::Dots(hitDots, 5, theme::kCream.Alpha(0.55f));
    if (n > 0) {
        sx /= n;
        sy /= n;
        const char* hDir = sx > 0.08f ? "right" : sx < -0.08f ? "left" : "";
        const char* vDir = sy > 0.08f ? "high" : sy < -0.08f ? "low" : "";
        std::string bias = (*vDir || *hDir) ? skaar::Va("Shots drift %s%s%s by %.2f radii.", vDir,
                                                        (*vDir && *hDir) ? " and " : "", hDir, std::hypot(sx, sy))
                                            : std::string("Shots centre on the target.");
        f.text.Draw(dcx, dcy + dr + 52, 22, bias, theme::kCreamSoft, 0, Align::kCenter);
    }

    kit::HLine(m, h - 104, w - 2 * m, theme::kHairline);
    f.mono.Draw(m, h - 64, theme::kMonoSize, "R  AGAIN     I  INSIGHTS     ESC  MENU", theme::kCreamSoft,
                theme::kTrackMono);
    if (!st.lastFile.empty()) {
        f.mono.Draw(w - m, h - 64, theme::kMonoSize, "SAVED  ·  " + kit::Upper(st.lastFile), theme::kCreamFaint,
                    theme::kTrackMono * 0.5f, Align::kRight);
    }
}

bool ReadFileForKit(const char* path, std::string& out) { return trap::FileRead(path, out); }

}  // namespace

// ---- hooks -----------------------------------------------------------------------

void Init() {
    st = State{};
    RegisterCvars();

    kit::Backend backend{};
    backend.registerShader = trap::R_RegisterShader;
    backend.setColor = trap::R_SetColor;
    backend.drawStretchPic = trap::R_DrawStretchPic;
    backend.readFile = ReadFileForKit;
    chart::Init(backend);

    st.model = trap::R_RegisterModel("models/skaar/target.md3");
    st.shader = trap::R_RegisterShader("skaar/target");
    st.hitSound = trap::S_RegisterSound("sound/feedback/hit.wav", false);
    st.killSound = trap::S_RegisterSound("sound/feedback/kill.wav", false);

    // The range and targets are authored as flat colours. The GL2 renderer's
    // auto-exposure pulls any light scene toward mid-grey and shifts brightness
    // as you look around, which an aim trainer can't have. Set every load,
    // since configs from earlier builds archive the old "1". Keep the tonemap
    // on: with r_hdr 1 (the default) and no tonemap, the scene blows out to white.
    trap::CvarSet("r_autoExposure", "0");
    trap::CvarSet("r_toneMap", "1");

    for (const char* cmd : {"sk_start", "sk_restart", "sk_stop", "sk_replay", "sk_insights"}) {
        trap::AddCommand(cmd);
    }

    // a replay that needed this map loaded first
    const std::string pending = trap::CvarString("sk_pendingReplay");
    if (!pending.empty()) {
        trap::CvarSet("sk_pendingReplay", "");
        st.phase = Phase::kWaitSpawn;
        st.replay.summary.file = pending;  // picked up once anchored
        LoadScenario(cv.scenario.string);
        return;
    }

    if (LoadScenario(cv.scenario.string) && st.scenario.map == cg.mapname) {
        st.phase = Phase::kWaitSpawn;
    } else {
        st.scenarioLoaded = st.scenarioLoaded && st.scenario.map == cg.mapname;
        SetFreeze(false);
    }
}

void Shutdown() {
    for (const char* cmd : {"sk_start", "sk_restart", "sk_stop", "sk_replay", "sk_insights"}) {
        trap::RemoveCommand(cmd);
    }
}

void OnCommand(const usercmd_t& cmd, const playerState_t& ps) {
    UpdateCvars();
    const skaar::Vec3 eye{ps.origin[0], ps.origin[1], ps.origin[2] + ps.viewheight};
    const float yaw = ps.viewangles[YAW], pitch = -ps.viewangles[PITCH];

    // A respawn asked for by Restart() lands a frame or two later; until then the
    // eye is still the one from before it. The server's Spawn() toggles
    // EF_TELEPORT_BIT, so the flip is the signal that the player is at the spawn.
    const bool spawned = !st.waitingSpawn || ((ps.eFlags ^ st.spawnEFlags) & EF_TELEPORT_BIT) != 0;
    if (st.phase == Phase::kWaitSpawn && spawned) {
        // the first command after the spawn: the player stands at the spawn
        st.waitingSpawn = false;
        st.anchor = eye;
        st.baseYaw = std::round(ps.viewangles[YAW]);
        if (!st.replay.summary.file.empty() && st.replay.samples.empty()) {
            const std::string file = st.replay.summary.file;
            st.replay = RunRecord{};
            st.phase = Phase::kOff;
            StartReplay(file);
        } else {
            BeginRun();
        }
    }

    const bool down = cmd.buttons & BUTTON_ATTACK;
    const float dt = st.lastCmdTime ? (cmd.serverTime - st.lastCmdTime) / 1000.0f : 0;
    st.lastCmdTime = cmd.serverTime;

    if (st.phase == Phase::kCountdown) {
        // the countdown only runs while no menu is open
        if (UiOpen()) {
            st.countdownEnd = cg.time + kCountdownMs;
        }
        if (cmd.serverTime >= st.countdownEnd) {
            st.phase = Phase::kRunning;
            st.runStart = cmd.serverTime;
        }
    }

    if (st.phase == Phase::kRunning) {
        if (UiOpen()) {
            // leaving for the menu abandons the run (it isn't saved); R starts a new one
            st.phase = Phase::kCountdown;
            BeginRun();
            st.attackWasDown = down;
            return;
        }
        const float t = (cmd.serverTime - st.runStart) / 1000.0f;
        // weapon scenarios: the player's movement widens the cone (counter-strafing matters)
        st.sim.SetMovement(std::hypot(ps.velocity[0], ps.velocity[1]), ps.groundEntityNum != ENTITYNUM_NONE,
                           ps.pm_flags & PMF_DUCKED);
        const skaar::TrainerEvents ev =
            st.sim.Input(t, eye, yaw, pitch, down && !st.attackWasDown, down, dt);

        while (st.nextSample <= t && st.nextSample <= st.scenario.duration) {
            st.samples.push_back({st.nextSample, yaw, pitch, eye.x, eye.y, eye.z});
            st.nextSample += 1.0f / skaar::kSampleHz;
        }

        if (st.sim.HasWeapon() && (ev.hits || ev.misses) && st.gunSound) {
            trap::S_StartLocalSound(st.gunSound, CHAN_WEAPON);
        }
        if (ev.kills) {
            st.killFlashUntil = cg.time + kKillFlashMs;
            if (cv.hitSound.integer) {
                trap::S_StartLocalSound(st.killSound, CHAN_LOCAL_SOUND);
            }
        } else if (ev.hits && st.scenario.fire == skaar::FireMode::kClick && cv.hitSound.integer) {
            trap::S_StartLocalSound(st.hitSound, CHAN_LOCAL_SOUND);
        }

        if (st.sim.Finished()) {
            SaveRun();
            st.phase = Phase::kSummary;
            SetFreeze(true);
        }
    }
    st.attackWasDown = down;
}

void AddEntities() {
    weapons::AddLocalEntities();  // arena effects (P6); one per-frame scene hook for both
    if (st.phase == Phase::kReplay) {
        AdvanceReplay();
    }
    if (st.phase != Phase::kCountdown && st.phase != Phase::kRunning && st.phase != Phase::kReplay) {
        return;
    }
    const float t = st.sim.Time();
    const float scale = st.sim.TargetRadius() / 64.0f;  // the model is a radius-64 sphere
    for (const skaar::Target& target : st.sim.Targets()) {
        if (!target.alive) {
            continue;
        }
        refEntity_t re{};
        re.reType = RT_MODEL;
        re.hModel = st.model;
        re.customShader = st.shader;
        re.renderfx = RF_NOSHADOW;
        const skaar::Vec3 p = st.sim.TargetPosition(target);
        re.origin[0] = p.x;
        re.origin[1] = p.y;
        re.origin[2] = p.z;
        VectorCopy(re.origin, re.oldorigin);
        AxisClear(re.axis);
        for (auto& row : re.axis) {
            VectorScale(row, scale, row);
        }
        re.nonNormalizedAxes = qtrue;

        Color c = kTarget;
        if (st.phase == Phase::kCountdown) {
            // visible to plan the first flick, washed toward the ground
            const Color g = theme::kStage;
            c = {c.r + (g.r - c.r) * 0.6f, c.g + (g.g - c.g) * 0.6f, c.b + (g.b - c.b) * 0.6f, 1};
        } else if (target.lastHitT >= 0 && t - target.lastHitT < kHitFlash) {
            c = kTargetHit;
        }
        re.shaderRGBA[0] = static_cast<byte>(c.r * 255);
        re.shaderRGBA[1] = static_cast<byte>(c.g * 255);
        re.shaderRGBA[2] = static_cast<byte>(c.b * 255);
        re.shaderRGBA[3] = 255;
        trap::R_AddRefEntityToScene(&re);
    }
}

void DrawHud() {
    if (!cvars.drawHud.integer || UiOpen()) {
        return;
    }
    switch (st.phase) {
        case Phase::kCountdown:
            DrawCountdown();
            DrawCrosshair();
            break;
        case Phase::kRunning:
            DrawRunHud(false);
            DrawCrosshair();
            break;
        case Phase::kReplay:
            DrawRunHud(true);
            DrawCrosshair();
            break;
        case Phase::kSummary:
            DrawSummary();
            break;
        default:
            if (!st.loadError.empty()) {
                kit::GetFonts().mono.Draw(theme::kMargin / 2, 80, theme::kMonoSize, "SCENARIO ERROR  ·  " + st.loadError,
                                          kTarget, theme::kTrackMono * 0.5f);
            }
            break;
    }
}

bool ConsoleCommand(const char* cmd) {
    if (!Q_stricmp(cmd, "sk_start")) {
        const std::string id = trap::Argc() > 1 ? trap::Argv(1) : std::string(cv.scenario.string);
        trap::CvarSet("sk_scenario", id.c_str());
        if (LoadScenario(id)) {
            // movement physics before the (re)start; the server follows g_movement
            std::string text;
            const bool known = skaar::move::FindRuleset(st.scenario.ruleset) >= 0 ||
                               trap::FileRead(skaar::Va("rulesets/%s.json", st.scenario.ruleset.c_str()), text);
            trap::CvarSet("g_movement", known ? st.scenario.ruleset.c_str() : "vq3");
            Restart();
        }
        return true;
    }
    if (!Q_stricmp(cmd, "sk_restart")) {
        // in the arena / tac range (no scenario), R reloads instead
        if (!st.scenarioLoaded && weapons::HudActive()) {
            trap::SendClientCommand("reload");
            return true;
        }
        if (st.phase == Phase::kReplay) {
            st.phase = Phase::kOff;
        }
        Restart();
        return true;
    }
    if (!Q_stricmp(cmd, "sk_stop")) {
        Stop();
        return true;
    }
    if (!Q_stricmp(cmd, "sk_replay")) {
        std::string file = trap::Argc() > 1 ? trap::Argv(1) : st.lastFile;
        if (file.empty()) {
            Com_Printf("usage: sk_replay <stats/runs/file.json>\n");
        } else {
            StartReplay(file);
        }
        return true;
    }
    if (!Q_stricmp(cmd, "sk_insights")) {
        trap::SendConsoleCommand(st.lastFile.empty() ? "skaar_ui insights\n"
                                                     : skaar::Va("skaar_ui run %s\n", st.lastFile.c_str()));
        return true;
    }
    return false;
}

bool Active() { return st.phase != Phase::kOff && st.phase != Phase::kWaitSpawn; }

void OverrideView(vec3_t origin, vec3_t angles) {
    if (st.phase == Phase::kRunning && st.sim.HasWeapon()) {
        // the gun's kick moves the camera (its cameraFollow share); bullets already include all of it
        float up, right;
        st.sim.Punch((cg.time - st.runStart) / 1000.0f, up, right);
        angles[PITCH] -= up;
        angles[YAW] -= right;
        return;
    }
    if (st.phase != Phase::kReplay) {
        return;
    }
    ReplayCamera(ReplayTime(), origin, angles);
}

}  // namespace cg::trainer
