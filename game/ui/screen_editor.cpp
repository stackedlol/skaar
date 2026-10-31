// Scenario editor: tune any scenario in game and save it as your own copy
// (scenarios/<id>-mine.json in the home dir, docs/scenarios.md). The built-in
// file is never changed.
//
// This file is part of Skaar. GPLv2 or later.
#include <algorithm>
#include <cmath>

#include "local.h"

namespace ui {
namespace {

constexpr float kRowStep = 54;
constexpr float kEndless = 1000;  // hold targets with this much health never die

std::string MineId(const std::string& id) {
    const std::string suffix = "-mine";
    return id.size() > suffix.size() && id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0 ? id
                                                                                                         : id + suffix;
}

std::string MineName(const std::string& name) {
    const std::string suffix = " (mine)";
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0
               ? name
               : name + suffix;
}

// A range edited as its centre; the spread around it is kept as a fraction.
// `minFloor` is the parser's own floor for the minimum: the slider moves the centre
// only, so staying above it can still leave a range the scenario file may not hold.
bool CentreSlider(float y, const char* label, skaar::Range& r, float lo, float hi, float minFloor, float step,
                  const char* format, const std::string& hint = "") {
    float centre = (r.min + r.max) / 2;
    const float spread = centre > 0 ? (r.max - r.min) / 2 / centre : 0;
    if (!ValueSlider(y, label, centre, lo, hi, step, format, hint)) {
        return false;
    }
    r.min = std::max(minFloor, centre * (1 - spread));
    r.max = centre * (1 + spread);
    return true;
}

// Write the draft as the player's copy, reload, and select it. Returns the id ("" on failure).
std::string Save() {
    skaar::Scenario& d = *ui.draft;
    const std::string id = MineId(d.id);
    d.name = MineName(d.name);
    std::string error;
    skaar::Scenario check;
    const std::string text = skaar::ScenarioToJson(d);
    if (!skaar::ParseScenario(id, text, check, error)) {
        ui.lastError = "Can't save: " + error;
        return "";
    }
    if (!trap::FileWrite(skaar::Va("scenarios/%s.json", id.c_str()), text)) {
        ui.lastError = "Can't write scenarios/" + id + ".json";
        return "";
    }
    LoadData();
    ui.selectedScenario = id;
    ui.category = "all";
    return id;
}

}  // namespace

void EditScenario(const std::string& id) {
    const skaar::Scenario* s = data.FindScenario(id);
    if (!s) {
        return;
    }
    ui.draft = *s;
    ui.lastError.clear();
    Open(Screen::kEditor);
}

void DrawEditor() {
    const kit::Fonts& f = kit::GetFonts();
    const float m = theme::kMargin, w = kit::Width(), h = kit::Height();
    if (!ui.draft) {
        Open(Screen::kPlay);
        return;
    }
    skaar::Scenario& d = *ui.draft;
    auto& t = d.targets;

    if (InGame()) {
        kit::Fill(0, 0, w, h, theme::kStage.Alpha(0.95f));
    } else {
        kit::Background(false);
    }
    f.display.Draw(m - 4, 150, 56, kit::Upper(MineName(d.name)), theme::kCream, theme::kTrackStatement);
    Label(m, 186, "SAVES AS YOUR OWN COPY   ·   SCENARIOS/" + kit::Upper(MineId(d.id)) + ".JSON", theme::kCreamFaint);

    SetRowHeight(kRowStep);
    float y = 214;
    ValueSlider(y, "Duration", d.duration, 10, 300, 5, "%.0f s");
    y += kRowStep;

    int fire = d.fire == skaar::FireMode::kHold ? 1 : 0;
    if (ValueChoice(y, "Fire", fire, {"Click", "Hold"}, fire ? "BEAM: SCORE IS TIME ON TARGET" : "ONE SHOT PER CLICK")) {
        d.fire = fire ? skaar::FireMode::kHold : skaar::FireMode::kClick;
        t.health = fire ? kEndless : 1;
    }
    y += kRowStep;

    float alive = static_cast<float>(t.alive);
    const int maxAlive = t.shape == skaar::SpawnShape::kGrid ? std::min(8, t.cols * t.rows - 1) : 8;
    if (ValueSlider(y, "Targets at once", alive, 1, static_cast<float>(maxAlive), 1, "%.0f")) {
        t.alive = static_cast<int>(alive);
    }
    y += kRowStep;

    float size = t.radius * 2;
    if (ValueSlider(y, "Target size", size, 8, 120, 2, "%.0f u", "THE PLAYER IS 30 WIDE")) {
        t.radius = size / 2;
    }
    y += kRowStep;

    if (d.fire == skaar::FireMode::kHold) {
        float hp = t.health >= kEndless ? 0 : t.health;
        if (ValueSlider(y, "Time to kill", hp, 0, 5, 0.05f, hp > 0 ? "%.2f s" : "endless", "0 = TARGETS NEVER DIE")) {
            t.health = hp > 0 ? hp : kEndless;
        }
    } else {
        ValueSlider(y, "Hits to kill", t.health, 1, 5, 1, "%.0f");
    }
    y += kRowStep;

    ValueSlider(y, "Lifetime", t.lifetime, 0, 5, 0.1f, t.lifetime > 0 ? "%.1f s" : "forever", "UNHIT TARGETS VANISH");
    y += kRowStep;

    CentreSlider(y, "Distance", t.distance, 150, 2000, 64, 25, "%.0f u");  // parser: distance >= 64
    y += kRowStep;

    float spread = (t.yaw.max - t.yaw.min) / 2;
    if (ValueSlider(y, "Spread", spread, 2, 90, 1, "%.0f°", "EACH SIDE OF CENTRE")) {
        const float centre = (t.yaw.min + t.yaw.max) / 2;
        t.yaw = {centre - spread, centre + spread};
    }
    y += kRowStep;

    int motion = static_cast<int>(t.motion);
    if (ValueChoice(y, "Movement", motion, {"Static", "Strafe", "Float"})) {
        t.motion = static_cast<skaar::MotionType>(motion);
        if (t.motion != skaar::MotionType::kStatic && t.speed.max <= 0) {
            t.speed = {200, 340};
        }
        if (t.motion != skaar::MotionType::kStatic && t.shape == skaar::SpawnShape::kGrid) {
            t.shape = skaar::SpawnShape::kCone;  // moving targets don't keep to grid cells
        }
    }
    y += kRowStep;

    if (t.motion != skaar::MotionType::kStatic) {
        CentreSlider(y, "Speed", t.speed, 20, 900, 0, 10, "%.0f u/s");
        y += kRowStep;
        CentreSlider(y, "Turns every", t.changeEvery, 0.2f, 3, 0.05f, 0.05f, "%.2f s");
        y += kRowStep;
    }

    int gun = d.weapon == "rifle" ? 1 : d.weapon == "handcannon" ? 2 : 0;
    if (ValueChoice(y, "Gun", gun, {"Trainer", "Rifle", "Hand cannon"}, gun ? "ITS RECOIL AND SPREAD" : "PERFECT AIM")) {
        d.weapon = gun == 1 ? "rifle" : gun == 2 ? "handcannon" : "";
    }
    y += kRowStep;

    int move = d.playerMove ? 1 : 0;
    if (ValueChoice(y, "You", move, {"Stand still", "Move"})) {
        d.playerMove = move != 0;
    }
    y += kRowStep;
    if (d.playerMove) {
        int physics = d.ruleset == "cpm" ? 1 : 0;
        physics = d.ruleset == "tac" ? 2 : physics;
        if (ValueChoice(y, "Movement physics", physics, {"vq3", "cpm", "tac"})) {
            d.ruleset = physics == 2 ? "tac" : physics ? "cpm" : "vq3";
        }
        y += kRowStep;
    }

    SetRowHeight(64);

    // actions
    const float by = std::max(y + 26, h - 150);
    if (ActionButton({m, by, 300, 64}, "Save and play", "")) {
        const std::string id = Save();
        if (!id.empty()) {
            ui.draft.reset();
            StartScenario(id);
            return;
        }
    }
    if (ActionButton({m + 320, by, 240, 64}, "Save", "")) {
        if (!Save().empty()) {
            ui.draft.reset();
            Open(Screen::kPlay);
            return;
        }
    }
    if (ActionButton({m + 580, by, 240, 64}, "Cancel", "ESC")) {
        ui.draft.reset();
        Open(Screen::kPlay);
        return;
    }
    if (!ui.lastError.empty()) {
        Label(m + 860, by + 40, ui.lastError, kEmber);
    }
    (void)TakeAdjust();
}

}  // namespace ui
