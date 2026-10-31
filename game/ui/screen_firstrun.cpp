// First run (Phase 2, E): three short steps before the menu: the mouse
// (dpi, sensitivity or a conversion, cm/360), the view (FOV, crosshair),
// then straight into a first scenario. Shown once (sk_firstRunDone).
//
// This file is part of Skaar. GPLv2 or later.
#include <cmath>

#include "../shared/reticle.h"
#include "../shared/sens.h"
#include "local.h"

namespace ui {
namespace {

constexpr float kRowStep = 64;
int g_step = 0;

void Finish() {
    trap::CvarSet("sk_firstRunDone", "1");
    g_step = 0;
}

void StepDots(float x, float y) {
    for (int i = 0; i < 3; i++) {
        kit::Fill(x + i * 22, y, 12, 3, i == g_step ? kEmber : theme::kHairlineStrong);
    }
}

void DrawMouse(float top) {
    const float sens = trap::CvarValue("sensitivity");
    // sens_dpi is the documented cvar (docs/feel.md, sens_test reads it); 0 means unset
    float dpi = trap::CvarValue("sens_dpi");
    if (dpi <= 0) {
        dpi = 800;
    }
    float v = 0;
    float y = top;
    if (NumberRow(y, "Mouse DPI", dpi, 50, 64000, "%.0f", v, "FROM YOUR MOUSE SOFTWARE")) {
        trap::CvarSet("sens_dpi", skaar::Va("%g", v));
    }
    y += kRowStep;
    if (NumberRow(y, "Sensitivity", sens, 0.001f, 100, "%.3f", v)) {
        trap::CvarSet("sensitivity", skaar::Va("%g", v));
    }
    y += kRowStep;
    // or bring it over from another game
    if (trap::CvarString("sk_convertYaw").empty()) {
        trap::CvarSet("sk_convertYaw", "0.022");
    }
    ChoiceRow(y, "Coming from a game with", "sk_convertYaw",
              {{"0.022", "Yaw 0.022"}, {"0.07", "Yaw 0.07"}, {"0.0066", "Yaw 0.0066"}}, "SEE DOCS/SENSITIVITY.MD");
    y += kRowStep;
    const float theirYaw = static_cast<float>(std::atof(trap::CvarString("sk_convertYaw").c_str()));
    if (NumberRow(y, "Your sensitivity there", trap::CvarValue("sk_convertSens"), 0.0001f, 1000, "%.3f", v,
                  "SETS YOURS HERE")) {
        trap::CvarSet("sk_convertSens", skaar::Va("%g", v));
        trap::CvarSet("sensitivity", skaar::Va("%.4f", skaar::sens::Convert(v, theirYaw)));
    }
    y += kRowStep + 30;
    const float cm = skaar::sens::CmPer360(trap::CvarValue("sensitivity"), skaar::sens::kOurYaw, dpi);
    kit::GetFonts().display.Draw(theme::kMargin - 4, y + 70, 64, skaar::Va("%.1f cm", cm), theme::kCream, -0.01f);
    Label(theme::kMargin, y + 104, "MOUSE TRAVEL FOR ONE FULL TURN", theme::kCreamFaint);
}

void DrawView(float top) {
    float y = top;
    SliderRow(y, "Field of view", "cg_fov", 60, 130, 1, "%.0f°", "HORIZONTAL");
    y += kRowStep;
    ChoiceRow(y, "Crosshair", "cg_crosshairStyle", {{"0", "Cross"}, {"1", "Dot"}, {"2", "Cross + dot"}});
    y += kRowStep;
    ChoiceRow(y, "Colour", "cg_crosshairColor",
              {{"f7e08a", "Pastel yellow"}, {"ffffff", "White"}, {"232323", "Graphite"}, {"3cff7a", "Green"},
               {"3ce8ff", "Cyan"}});
    y += kRowStep;
    SliderRow(y, "Size", "cg_crosshairSize", 0, 16, 1, "%.0f");
    // preview on a patch of range
    const float w = kit::Width(), m = theme::kMargin;
    const Rect r{w - m - 360, top + kRowStep * 4 + 40, 360, 220};
    kit::Fill(r.x, r.y, r.w, r.h, theme::kCream.Alpha(0.06f));
    chart::Disc(r.x + r.w / 2 + 60, r.y + r.h / 2 - 40, 22, kEmber);
    const float s = kit::Scale();
    kit::DrawReticle(std::floor((r.x + r.w / 2) * s) / s, std::floor((r.y + r.h / 2) * s) / s,
                     kit::ReticleFromCvars(trap::CvarString));
}

void DrawReady(float top) {
    const float m = theme::kMargin;
    Paragraph(m, top, 760,
              "That's it. Start with Wall grid: three targets on a wall, sixty seconds. Every run you finish lands "
              "in Insights, and R starts the next one.",
              type::kBody, theme::kCreamSoft);
    if (ActionButton({m, top + 140, 420, 72}, "Play Wall grid", "ENTER")) {
        Finish();
        StartScenario("wall-grid");
        return;
    }
    if (ActionButton({m + 440, top + 140, 300, 72}, "Go to the menu", "")) {
        Finish();
        Open(Screen::kHome);
    }
}

}  // namespace

bool FirstRunPending() { return trap::CvarValue("sk_firstRunDone") == 0; }

void DrawFirstRun() {
    const kit::Fonts& f = kit::GetFonts();
    const float m = theme::kMargin, h = kit::Height();
    kit::Background(false);
    static const char* titles[] = {"YOUR MOUSE.", "YOUR VIEW.", "READY."};
    Label(m, 84, skaar::Va("SETUP   ·   %d / 3", g_step + 1), theme::kCreamSoft);
    f.display.Draw(m - 6, 250, type::kHero, titles[g_step], theme::kCream, theme::kTrackStatement);
    StepDots(m, 120);

    const float top = 330;
    switch (g_step) {
        case 0:
            DrawMouse(top);
            break;
        case 1:
            DrawView(top);
            break;
        default:
            DrawReady(top);
            return;
    }
    if (ActionButton({m, h - 190, 300, 64}, "Next", "→")) {
        g_step++;
        ui.focus = 0;
        ResetMarker();
    }
    if (g_step > 0 && ActionButton({m + 320, h - 190, 240, 64}, "Back", "")) {
        g_step--;
        ui.focus = 0;
        ResetMarker();
    }
    if (ActionButton({kit::Width() - m - 240, h - 190, 240, 64}, "Skip setup", "")) {
        Finish();
        Open(Screen::kHome);
    }
}

}  // namespace ui
