// Settings: aim (sensitivity in cm/360, conversion from other games' yaw,
// FOV), crosshair (with a live preview), video and audio.
//
// This file is part of Skaar. GPLv2 or later.
#include <cctype>
#include <cmath>
#include <map>
#include <cstdio>

#include "../shared/reticle.h"
#include "../shared/sens.h"
#include "local.h"

namespace ui {
namespace {

constexpr float kRowStep = 64;

float CmPer360(float sensitivity, float yaw, float dpi) { return skaar::sens::CmPer360(sensitivity, yaw, dpi); }

void DrawAim(float top) {
    const kit::Fonts& f = kit::GetFonts();
    const float m = theme::kMargin;
    const float sens = trap::CvarValue("sensitivity");
    const float yaw = trap::CvarValue("m_yaw") > 0 ? trap::CvarValue("m_yaw") : 0.022f;
    float dpi = trap::CvarValue("sk_dpi");
    if (dpi <= 0) {
        dpi = 800;
    }
    const float cm = CmPer360(sens, yaw, dpi);
    float y = top;
    float v = 0;

    if (NumberRow(y, "Sensitivity", sens, 0.001f, 100, "%.3f", v)) {
        trap::CvarSet("sensitivity", skaar::Va("%g", v));
    }
    y += kRowStep;
    if (NumberRow(y, "Mouse DPI", dpi, 50, 64000, "%.0f", v)) {
        trap::CvarSet("sk_dpi", skaar::Va("%g", v));
    }
    y += kRowStep;
    if (NumberRow(y, "cm / 360°", cm, 1, 500, "%.2f", v)) {
        // sensitivity giving v cm per 360 at this dpi
        trap::CvarSet("sensitivity", skaar::Va("%.4f", skaar::sens::SensitivityFor(v, dpi > 0 ? yaw : 0, dpi)));
    }
    y += kRowStep;

    // conversion from another game: same deg/count = same feel
    // yaw values with community-documented standards (docs/sensitivity.md); no game names in the product
    static const std::vector<std::pair<std::string, std::string>> yaws = {
        {"0.022", "Yaw 0.022"}, {"0.07", "Yaw 0.07"}, {"0.0066", "Yaw 0.0066"}};
    if (trap::CvarString("sk_convertYaw").empty()) {
        trap::CvarSet("sk_convertYaw", "0.022");
    }
    ChoiceRow(y, "Convert from", "sk_convertYaw", yaws, "THE OTHER GAME'S YAW");
    y += kRowStep;
    const float otherYaw = std::atof(trap::CvarString("sk_convertYaw").c_str());
    const float otherSens = trap::CvarValue("sk_convertSens");
    const float converted = otherYaw > 0 ? skaar::sens::Convert(otherSens, otherYaw, yaw) : 0;
    if (NumberRow(y, "Their sensitivity", otherSens, 0.0001f, 1000, "%.3f", v,
                  otherSens > 0 ? skaar::Va("= %.3f HERE  ·  %.2f CM/360", converted,
                                            CmPer360(converted, yaw, dpi))
                                : "")) {
        trap::CvarSet("sk_convertSens", skaar::Va("%g", v));
    }
    y += kRowStep;
    if (ActionButton({m, y + 14, 420, 56}, "Apply conversion", converted > 0 ? skaar::Va("→ %.3f", converted) : "")) {
        if (converted > 0) {
            trap::CvarSet("sensitivity", skaar::Va("%.4f", converted));
        }
    }
    y += kRowStep + 30;

    SliderRow(y, "Field of view", "cg_fov", 60, 130, 1, "%.0f°", "HORIZONTAL");
    y += kRowStep + 40;

    Label(m, y, "RAW INPUT, NO ACCELERATION   ·   CHECK A 360 WITH SENS_TEST", theme::kCreamFaint);
    (void)f;
}

void DrawCrosshairPreview(const Rect& r) {
    // a patch of range: the ground tinted a touch towards the ink, like the range walls
    kit::Fill(r.x, r.y, r.w, r.h, theme::kStage);
    kit::Fill(r.x, r.y, r.w, r.h, theme::kCream.Alpha(0.06f));
    kit::HLine(r.x, r.y, r.w, theme::kHairlineStrong);
    kit::HLine(r.x, r.Bottom(), r.w, theme::kHairlineStrong);
    kit::VLine(r.x, r.y, r.h, theme::kHairlineStrong);
    kit::VLine(r.Right(), r.y, r.h, theme::kHairlineStrong);
    // a target just off centre, as in a run
    chart::Disc(r.x + r.w / 2 + 70, r.y + r.h / 2 - 46, 26, kEmber);
    Label(r.x + 20, r.Bottom() - 20, "PREVIEW", theme::kCreamFaint);

    // the same reticle the game draws (game/shared/reticle.h), centred on a whole pixel
    const float s = kit::Scale();
    const float cx = std::floor((r.x + r.w / 2) * s) / s, cy = std::floor((r.y + r.h / 2) * s) / s;
    kit::DrawReticle(cx, cy, kit::ReticleFromCvars(trap::CvarString));
}

void DrawCrosshair(float top) {
    const float m = theme::kMargin, w = kit::Width();
    float y = top;
    ChoiceRow(y, "Style", "cg_crosshairStyle", {{"0", "Cross"}, {"1", "Dot"}, {"2", "Cross + dot"}});
    y += kRowStep;
    SliderRow(y, "Length", "cg_crosshairSize", 0, 16, 1, "%.0f");
    y += kRowStep;
    SliderRow(y, "Gap", "cg_crosshairGap", 0, 10, 1, "%.0f");
    y += kRowStep;
    SliderRow(y, "Thickness", "cg_crosshairThickness", 1, 6, 0.5f, "%.1f");
    y += kRowStep;
    ToggleRow(y, "Outline", "cg_crosshairOutline");
    y += kRowStep;
    ChoiceRow(y, "Colour", "cg_crosshairColor",
              {{"f7e08a", "Pastel yellow"}, {"ffffff", "White"}, {"232323", "Graphite"}, {"3cff7a", "Green"},
               {"3ce8ff", "Cyan"}, {"ff4df0", "Magenta"}});
    y += kRowStep;
    DrawCrosshairPreview({w - m - 420, top + kRowStep * 7 + 30, 420, 260});
    (void)y;
}

void DrawControls(float top) {
    const kit::Fonts& f = kit::GetFonts();
    const float m = theme::kMargin, w = kit::Width();
    struct Action {
        const char* label;
        const char* command;
    };
    static const Action actions[] = {
        {"Fire", "+attack"},        {"Restart run", "sk_restart"}, {"Insights", "sk_insights"},
        {"Forward", "+forward"},    {"Back", "+back"},             {"Left", "+moveleft"},
        {"Right", "+moveright"},    {"Jump", "+moveup"},           {"Crouch", "+movedown"},
        {"Walk", "+speed"},         {"Next weapon", "weapnext"},   {"Previous weapon", "weapprev"},
        {"Rail", "weapon 7"},       {"Rocket", "weapon 5"},        {"Lightning", "weapon 6"},
        {"Shotgun", "weapon 3"},
    };
    // which keys run which command, once per frame
    std::map<std::string, std::vector<std::string>> keys;
    for (int k = 0; k < MAX_KEYS; k++) {
        std::string b = trap::KeyBinding(k);
        if (!b.empty()) {
            for (char& c : b) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            keys[b].push_back(trap::KeyName(k));
        }
    }

    // two columns: actions and movement on the left, weapons on the right
    const float colW = (w - 2 * m - 120) / 2;
    const int perColumn = 10;
    int index = 0;
    for (const Action& a : actions) {
        const float x = m + (index / perColumn) * (colW + 120);
        const float y = top + (index % perColumn) * kRowStep;
        index++;
        const Rect r{x - 40, y, colW + 40, kRowStep};
        const int id = Focusable(r);
        const bool focused = Focused(id);
        const float baseline = y + kRowStep / 2 + 9;
        if (focused) {
            Marker(x - 28, baseline, f.text.CapHeight(type::kRow));
        }
        f.text.Draw(x, baseline, type::kRow, a.label, focused ? theme::kCream : theme::kCreamSoft);
        std::string bound;
        for (const std::string& k : keys[a.command]) {
            bound += (bound.empty() ? "" : "   ") + kit::Upper(k);
        }
        const bool waiting = ui.capturing == a.command;
        f.mono.Draw(x + 320, baseline, 24, waiting ? "PRESS A KEY" : bound.empty() ? "—" : bound,
                    waiting ? kEmber : theme::kCream, 0.06f);
        if (focused && !waiting) {
            for (int key : in.keys) {
                if (key == K_BACKSPACE || key == K_DEL) {
                    for (int k = 0; k < MAX_KEYS; k++) {
                        if (!Q_stricmp(trap::KeyBinding(k).c_str(), a.command)) {
                            trap::SetBinding(k, "");
                        }
                    }
                }
            }
        }
        kit::HLine(x, y + kRowStep, colW, theme::kHairline);
        if (Activated(id, r)) {
            ui.capturing = a.command;
        }
        Adjusted(id);  // Left/Right don't switch tabs from here
    }
    Label(m, top + perColumn * kRowStep + 40, "ENTER  BIND     BACKSPACE  CLEAR", theme::kCreamFaint);
}

void DrawGame(float top) {
    float y = top;
    // movement physics per mode; the menu sets g_movement from these when it starts the mode
    const std::vector<std::pair<std::string, std::string>> physics = {{"vq3", "Classic (vq3)"}, {"cpm", "Advanced (cpm)"}};
    if (trap::CvarString("sk_arenaMovement").empty()) {
        trap::CvarSet("sk_arenaMovement", "cpm");
    }
    if (trap::CvarString("sk_roamMovement").empty()) {
        trap::CvarSet("sk_roamMovement", "cpm");
    }
    ChoiceRow(y, "Arena movement", "sk_arenaMovement", physics, "STRAFE JUMPS, AIR CONTROL, DOUBLE JUMPS IN CPM");
    y += kRowStep;
    ChoiceRow(y, "Free roam movement", "sk_roamMovement", physics);
    y += kRowStep;
    ChoiceRow(y, "Arena loadout", "g_loadout", {{"1", "Every weapon"}, {"0", "Pick them up"}});
    y += kRowStep;
    ToggleRow(y, "Infinite ammo", "g_infiniteAmmo", "ARENA AND TAC RANGE");
    y += kRowStep;
    Label(theme::kMargin, y + 40, "SCENARIOS SET THEIR OWN MOVEMENT · THE TAC RANGE USES TAC MOVEMENT", theme::kCreamFaint);
}

void DrawVideo(float top) {
    const float m = theme::kMargin;
    float y = top;
    // both apply instantly, no video restart
    ToggleRow(y, "Fullscreen", "r_fullscreen", "CMD+CTRL+F");
    y += kRowStep;
    ToggleRow(y, "Retina resolution", "r_allowHighDPI", "NEEDS APPLY");
    y += kRowStep;
    ToggleRow(y, "Vertical sync", "r_swapInterval");
    y += kRowStep;
    ChoiceRow(y, "Frame rate cap", "com_maxfps",
              {{"0", "Uncapped"}, {"144", "144"}, {"240", "240"}, {"360", "360"}, {"500", "500"}, {"1000", "1000"}});
    y += kRowStep;
    SliderRow(y, "Brightness", "r_gamma", 0.8f, 1.6f, 0.05f, "%.2f", "");
    y += kRowStep + 30;
    if (ActionButton({m, y, 420, 64}, "Apply video changes", "RESTARTS VIDEO")) {
        Exec("vid_restart\n");
    }
}

void DrawAudio(float top) {
    float y = top;
    SliderRow(y, "Volume", "s_volume", 0, 1, 0.05f, "%.2f");
    y += kRowStep;
    ToggleRow(y, "Hit and kill sounds", "sk_hitSound");
}

}  // namespace

void DrawSettings() {
    const float m = theme::kMargin, w = kit::Width(), h = kit::Height();
    if (InGame()) {
        kit::Fill(0, 0, w, h, theme::kStage.Alpha(0.95f));
    } else {
        kit::Background(true);
    }
    Header("");

    struct T {
        const char* label;
        SettingsTab tab;
    };
    const T tabs[] = {{"Aim", SettingsTab::kAim},
                      {"Crosshair", SettingsTab::kCrosshair},
                      {"Controls", SettingsTab::kControls},
                      {"Game", SettingsTab::kGame},
                      {"Video", SettingsTab::kVideo},
                      {"Audio", SettingsTab::kAudio}};
    float x = m;
    for (const T& t : tabs) {
        float tw = 0;
        if (Tab(x, 186, t.label, ui.settingsTab == t.tab, &tw)) {
            ui.settingsTab = t.tab;
        }
        x += tw + 44;
    }

    const float top = 240;
    switch (ui.settingsTab) {
        case SettingsTab::kAim:
            DrawAim(top);
            break;
        case SettingsTab::kCrosshair:
            DrawCrosshair(top);
            break;
        case SettingsTab::kControls:
            DrawControls(top);
            break;
        case SettingsTab::kGame:
            DrawGame(top);
            break;
        case SettingsTab::kVideo:
            DrawVideo(top);
            break;
        case SettingsTab::kAudio:
            DrawAudio(top);
            break;
    }
    // Left/Right on a row that doesn't take it (buttons): next/previous tab
    if (const int dir = TakeAdjust()) {
        const int n = 6, i = static_cast<int>(ui.settingsTab);
        ui.settingsTab = static_cast<SettingsTab>((i + dir + n) % n);
        ui.focus = 0;
        ResetMarker();
    }
    Footer(ui.editing >= 0 ? "ENTER  SAVE     ESC  CANCEL" : "← →  CHANGE     ESC  BACK");
}

}  // namespace ui
