// Immediate-mode widgets: focus, buttons, tabs, cells, settings rows, text.
//
// This file is part of Skaar. GPLv2 or later.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "local.h"

namespace ui {
namespace {

bool g_editCommit = false;
bool g_editCancel = false;
bool g_navigated = false;  // the focus moved by keyboard this frame
int g_dragging = -1;       // focus id of the slider being dragged

float kRowH = 64;  // settings-style rows; the editor packs them tighter (SetRowHeight)
constexpr float kControlX = 560;  // settings control column, from the margin

int g_lastFocus = -1;
int g_lastMoveSound = 0;

// The selection marker: one ember bar that slides to whatever has focus
struct MarkerAnim {
    bool has = false, target = false;
    float x = 0, y = 0, h = 0;      // drawn
    float tx = 0, ty = 0, th = 0;   // wanted this frame
    int lastTime = 0;
} g_marker;

void Tick() {
    if (ui.confirm) {
        trap::S_StartLocalSound(ui.confirm, CHAN_LOCAL_SOUND);
    }
}

void FocusTick(float x, float baseline, float cap) { Marker(x, baseline, cap); }

}  // namespace

// ---- frame / focus ---------------------------------------------------------------

void Marker(float x, float baseline, float cap) {
    g_marker.target = true;
    g_marker.tx = x;
    g_marker.ty = baseline - cap;
    g_marker.th = cap;
}

void ResetMarker() { g_marker.has = false; }

void DrawMarker() {
    if (!g_marker.target) {
        g_marker.has = false;
        return;
    }
    const int dt = std::clamp(ui.realtime - g_marker.lastTime, 0, 100);
    g_marker.lastTime = ui.realtime;
    // snap on a new screen or a jump between columns; otherwise ease out (~90 ms to settle)
    if (!g_marker.has || std::fabs(g_marker.tx - g_marker.x) > 80) {
        g_marker.x = g_marker.tx;
        g_marker.y = g_marker.ty;
        g_marker.h = g_marker.th;
        g_marker.has = true;
    } else {
        const float k = 1.0f - std::exp(-dt / 28.0f);
        g_marker.x += (g_marker.tx - g_marker.x) * k;
        g_marker.y += (g_marker.ty - g_marker.y) * k;
        g_marker.h += (g_marker.th - g_marker.h) * k;
    }
    kit::Fill(g_marker.x, g_marker.y, 4, g_marker.h, kEmber);
    g_marker.target = false;
}

void SetRowHeight(float h) { kRowH = h; }

int TakeAdjust() {
    const int a = ui.adjust;
    ui.adjust = 0;
    return a;
}

void BeginFrame() {
    ui.prevFocusCount = ui.focusCount;
    ui.focusCount = 0;
    ui.activate = false;
    ui.adjust = 0;
    ui.back = false;
    g_editCommit = g_editCancel = false;
    g_navigated = false;

    if (ui.editing >= 0) {
        for (char c : in.chars) {
            if ((c >= '0' && c <= '9') || c == '.' || (c == '-' && ui.editBuffer.empty())) {
                if (ui.editBuffer.size() < 12) {
                    ui.editBuffer += c;
                }
            }
        }
        for (int key : in.keys) {
            if (key == K_BACKSPACE && !ui.editBuffer.empty()) {
                ui.editBuffer.pop_back();
            } else if (key == K_ENTER || key == K_KP_ENTER) {
                g_editCommit = true;
            } else if (key == K_ESCAPE) {
                g_editCancel = true;
            }
        }
        // clicking elsewhere commits too
        if (in.click) {
            g_editCommit = true;
        }
        return;
    }

    const int count = std::max(1, ui.prevFocusCount);
    for (int key : in.keys) {
        switch (key) {
            case K_UPARROW:
            case K_KP_UPARROW:
                ui.focus = (ui.focus + count - 1) % count;
                g_navigated = true;
                break;
            case K_DOWNARROW:
            case K_KP_DOWNARROW:
            case K_TAB:
                ui.focus = (ui.focus + 1) % count;
                g_navigated = true;
                break;
            case K_LEFTARROW:
            case K_KP_LEFTARROW:
                ui.adjust = -1;
                break;
            case K_RIGHTARROW:
            case K_KP_RIGHTARROW:
                ui.adjust = 1;
                break;
            case K_ENTER:
            case K_KP_ENTER:
            case K_SPACE:
                ui.activate = true;
                break;
            case K_ESCAPE:
                ui.back = true;
                break;
            default:
                break;
        }
    }
    ui.focus = std::clamp(ui.focus, 0, count - 1);
    if (!in.mouseDown) {
        g_dragging = -1;
    }
}

void EndFrame() {
    if (ui.focus != g_lastFocus) {
        if (g_lastFocus >= 0 && ui.move && ui.realtime - g_lastMoveSound > 35) {
            trap::S_StartLocalSound(ui.move, CHAN_LOCAL_SOUND);
            g_lastMoveSound = ui.realtime;
        }
        g_lastFocus = ui.focus;
    }
    in.keys.clear();
    in.chars.clear();
    in.click = false;
    in.wheel = 0;
    in.mouseMoved = false;
}

int Focusable(const Rect& r) {
    const int id = ui.focusCount++;
    if (in.mouseMoved && ui.editing < 0 && r.Contains(in.mx, in.my)) {
        ui.focus = id;
    }
    return id;
}

bool Focused(int id) { return ui.focus == id; }

bool Hovered(const Rect& r) { return in.mouseUsed && r.Contains(in.mx, in.my); }

bool Activated(int id, const Rect& r) {
    if (ui.editing >= 0) {
        return false;
    }
    if (in.click && r.Contains(in.mx, in.my)) {
        ui.focus = id;
        in.click = false;  // one widget per click
        Tick();
        return true;
    }
    if (ui.activate && ui.focus == id) {
        ui.activate = false;
        Tick();
        return true;
    }
    return false;
}

int Adjusted(int id) {
    if (ui.editing < 0 && ui.focus == id && ui.adjust) {
        const int a = ui.adjust;
        ui.adjust = 0;
        return a;
    }
    return 0;
}

float Scroll(const std::string& key, const Rect& view, float contentHeight, float focusTop, float focusBottom) {
    float& s = ui.scroll[key];
    if (in.wheel && Hovered(view)) {
        s -= in.wheel * 72.0f;
        in.wheel = 0;
    }
    // keep the keyboard focus in view
    if (g_navigated && focusTop >= 0) {
        if (focusTop - s < 0) {
            s = focusTop;
        } else if (focusBottom - s > view.h) {
            s = focusBottom - view.h;
        }
    }
    s = std::clamp(s, 0.0f, std::max(0.0f, contentHeight - view.h));
    return s;
}

// ---- text ------------------------------------------------------------------------

void Label(float x, float baseline, const std::string& text, Color color, Align align) {
    kit::GetFonts().mono.Draw(x, baseline, type::kLabel, kit::Upper(text), color, theme::kTrackMono, align);
}

std::vector<std::string> Wrap(const kit::Font& font, float size, const std::string& text, float width) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flushWord = [&] {
        if (word.empty()) {
            return;
        }
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && font.Measure(size, candidate) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == ' ') {
            flushWord();
        } else if (c == '\n') {
            flushWord();
            lines.push_back(line);
            line.clear();
        } else {
            word += c;
        }
    }
    flushWord();
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

float Paragraph(float x, float y, float width, const std::string& text, float size, Color color, float lineHeight) {
    const kit::Font& f = kit::GetFonts().text;
    float baseline = y + size;
    for (const std::string& line : Wrap(f, size, text, width)) {
        f.Draw(x, baseline, size, line, color);
        baseline += size * lineHeight;
    }
    return baseline - size * lineHeight;  // last baseline
}

// ---- chrome ----------------------------------------------------------------------

bool Tab(float x, float baseline, const std::string& label, bool active, float* outWidth) {
    const kit::Font& f = kit::GetFonts().text;
    const std::string text = kit::Upper(label);
    const float w = f.Measure(type::kNav, text, 0.14f);
    const Rect r{x - 14, baseline - 34, w + 28, 50};
    const int id = Focusable(r);
    const bool focused = Focused(id);
    f.Draw(x, baseline, type::kNav, text, active || focused ? theme::kCream : theme::kCreamSoft, 0.14f);
    if (active) {
        kit::Fill(x, baseline + 12, w, 2, kEmber);
    } else if (focused) {
        kit::Fill(x, baseline + 12, w, 1, theme::kCreamSoft);
    }
    if (outWidth) {
        *outWidth = w;
    }
    return Activated(id, r);
}

void Header(const std::string& eyebrowLeft) {
    const float m = theme::kMargin, w = kit::Width();
    kit::Image(m - 6, 50, 52, 52, ui.mark, theme::kCream);

    struct Nav {
        const char* label;
        Screen screen;
    };
    const Nav navs[] = {{"Play", Screen::kPlay}, {"Insights", Screen::kInsights}, {"Settings", Screen::kSettings}};
    float x = m + 84;
    for (const Nav& n : navs) {
        float tw = 0;
        if (Tab(x, 86, n.label, ui.screen == n.screen, &tw) && ui.screen != n.screen) {
            Open(n.screen);
        }
        x += tw + 56;
    }
    if (!eyebrowLeft.empty()) {
        Label(w - m, 84, eyebrowLeft, theme::kCreamFaint, Align::kRight);
    }
}

void Footer(const std::string& hints) {
    const float m = theme::kMargin, h = kit::Height();
    Label(m, h - 56, hints, theme::kCreamFaint);
}

void Pointer() {
    if (in.mouseUsed && !in.systemCursor) {
        kit::Image(in.mx, in.my, 26, 26, ui.pointer, theme::kCream);
    }
}

bool ActionButton(const Rect& r, const std::string& label, const std::string& hint) {
    const int id = Focusable(r);
    const bool focused = Focused(id);
    const kit::Fonts& f = kit::GetFonts();
    kit::HLine(r.x, r.y, r.w, focused ? theme::kCreamSoft : theme::kHairlineStrong);
    kit::HLine(r.x, r.Bottom() - 1, r.w, focused ? theme::kCreamSoft : theme::kHairlineStrong);
    kit::VLine(r.x, r.y, r.h, focused ? theme::kCreamSoft : theme::kHairlineStrong);
    kit::VLine(r.Right() - 1, r.y, r.h, focused ? theme::kCreamSoft : theme::kHairlineStrong);
    const float baseline = r.y + r.h / 2 + f.text.CapHeight(type::kNav) / 2;
    if (focused) {
        FocusTick(r.x + 20, baseline, f.text.CapHeight(type::kNav));
    }
    f.text.Draw(r.x + 40, baseline, type::kNav, kit::Upper(label), theme::kCream, 0.14f);
    if (!hint.empty()) {
        Label(r.Right() - 22, baseline - 1, hint, theme::kCreamFaint, Align::kRight);
    }
    return Activated(id, r);
}

void Cell(const Rect& r, const std::string& label, const std::string& value, Color valueColor, float valueSize) {
    Label(r.x + 22, r.y + 36, label);
    kit::GetFonts().mono.Draw(r.x + 22, r.y + 36 + 14 + valueSize, valueSize, value, valueColor, 0.01f);
}

void CellGrid(float x, float y, float cellW, float cellH, int cols, int count) {
    const int rows = (count + cols - 1) / cols;
    for (int r = 0; r <= rows; r++) {
        kit::HLine(x, y + r * cellH, cellW * cols, theme::kHairline);
    }
    for (int c = 0; c <= cols; c++) {
        kit::VLine(x + c * cellW, y, cellH * rows, theme::kHairline);
    }
}

std::string FormatDuration(float seconds) {
    const int s = static_cast<int>(seconds + 0.5f);
    if (s >= 3600) {
        return skaar::Va("%d h %02d m", s / 3600, (s / 60) % 60);
    }
    return skaar::Va("%d:%02d", s / 60, s % 60);
}

std::string FormatDate(const skaar::RunTime& t, const skaar::RunTime& now) {
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int64_t days = now.DayNumber() - t.DayNumber();
    if (days == 0) {
        return skaar::Va("Today %02d:%02d", t.hour, t.minute);
    }
    if (days == 1) {
        return skaar::Va("Yesterday %02d:%02d", t.hour, t.minute);
    }
    return skaar::Va("%s %d  %02d:%02d", months[std::clamp(t.month, 1, 12) - 1], t.day, t.hour, t.minute);
}

// ---- settings rows ---------------------------------------------------------------

namespace {

struct RowLayout {
    int id;
    bool focused;
    float baseline;
    float controlX;
};

RowLayout BeginRow(float y, const char* label, const std::string& hint) {
    const float m = theme::kMargin;
    const Rect r{m - 40, y, kit::Width() - 2 * m + 40, kRowH};
    RowLayout row;
    row.id = Focusable(r);
    row.focused = Focused(row.id);
    row.baseline = y + kRowH / 2 + 9;
    row.controlX = m + kControlX;
    const kit::Font& f = kit::GetFonts().text;
    if (row.focused) {
        FocusTick(m - 28, row.baseline, f.CapHeight(type::kRow));
    }
    f.Draw(m, row.baseline, type::kRow, label, row.focused ? theme::kCream : theme::kCreamSoft);
    if (!hint.empty()) {
        Label(kit::Width() - m, row.baseline - 2, hint, theme::kCreamFaint, Align::kRight);
    }
    kit::HLine(m, y + kRowH, kit::Width() - 2 * m, theme::kHairline);
    return row;
}

std::string FormatValue(const char* format, float v) {
    return skaar::Va(format, v);
}

}  // namespace

bool ValueSlider(float y, const char* label, float& v, float lo, float hi, float step, const char* format,
                 const std::string& hint) {
    RowLayout row = BeginRow(y, label, hint);
    const float trackW = 420, trackX = row.controlX, trackY = row.baseline - 9;
    const Rect track{trackX - 10, y + 8, trackW + 20, kRowH - 16};

    bool changed = false;
    if (const int a = Adjusted(row.id)) {
        v = std::clamp(std::round((v + a * step) / step) * step, lo, hi);
        changed = true;
    }
    if (in.click && track.Contains(in.mx, in.my)) {
        g_dragging = row.id;
        in.click = false;
        ui.focus = row.id;
    }
    if (g_dragging == row.id && in.mouseDown) {
        const float f = std::clamp((in.mx - trackX) / trackW, 0.0f, 1.0f);
        const float nv = std::clamp(std::round((lo + f * (hi - lo)) / step) * step, lo, hi);
        if (nv != v) {
            v = nv;
            changed = true;
        }
    }

    const float frac = hi > lo ? std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) : 0;
    kit::Fill(trackX, trackY, trackW, 2, theme::kHairlineStrong);
    kit::Fill(trackX, trackY, trackW * frac, 2, row.focused ? theme::kCream : theme::kCreamSoft);
    kit::Fill(trackX + trackW * frac - 2, trackY - 9, 4, 20, row.focused ? theme::kCream : theme::kCreamSoft);
    kit::GetFonts().mono.Draw(trackX + trackW + 40, row.baseline, 24, FormatValue(format, v), theme::kCream);
    return changed;
}

bool SliderRow(float y, const char* label, const char* cvar, float lo, float hi, float step, const char* format,
               const std::string& hint) {
    float v = trap::CvarValue(cvar);
    if (ValueSlider(y, label, v, lo, hi, step, format, hint)) {
        trap::CvarSet(cvar, skaar::Va("%g", v));
        return true;
    }
    return false;
}

bool ValueChoice(float y, const char* label, int& index, const std::vector<std::string>& choices,
                 const std::string& hint) {
    RowLayout row = BeginRow(y, label, hint);
    const Rect r{theme::kMargin - 40, y, kit::Width() - 2 * theme::kMargin + 40, kRowH};
    const int n = static_cast<int>(choices.size());
    int dir = Adjusted(row.id);
    if (!dir && Activated(row.id, r)) {
        dir = 1;
    }
    if (n == 0) {
        return false;
    }
    index = std::clamp(index, 0, n - 1);
    if (dir) {
        index = (index + dir + n) % n;
    }
    const kit::Font& mono = kit::GetFonts().mono;
    const Color arrow = row.focused ? theme::kCreamSoft : theme::kCreamFaint;
    mono.Draw(row.controlX, row.baseline, 24, "←", arrow);
    mono.Draw(row.controlX + 44, row.baseline, 24, kit::Upper(choices[index]), theme::kCream, 0.08f);
    const float w = mono.Measure(24, kit::Upper(choices[index]), 0.08f);
    mono.Draw(row.controlX + 44 + w + 24, row.baseline, 24, "→", arrow);
    return dir != 0;
}

bool ToggleRow(float y, const char* label, const char* cvar, const std::string& hint) {
    RowLayout row = BeginRow(y, label, hint);
    const Rect r{theme::kMargin - 40, y, kit::Width() - 2 * theme::kMargin + 40, kRowH};
    bool on = trap::CvarValue(cvar) != 0;
    bool changed = false;
    if (Adjusted(row.id) || Activated(row.id, r)) {
        on = !on;
        trap::CvarSet(cvar, on ? "1" : "0");
        changed = true;
    }
    const kit::Font& mono = kit::GetFonts().mono;
    mono.Draw(row.controlX, row.baseline, 24, "ON", on ? theme::kCream : theme::kCreamFaint, 0.1f);
    mono.Draw(row.controlX + 80, row.baseline, 24, "OFF", on ? theme::kCreamFaint : theme::kCream, 0.1f);
    kit::Fill(on ? row.controlX : row.controlX + 80, row.baseline + 10, on ? 30 : 46, 2,
              row.focused ? theme::kCream : theme::kCreamSoft);
    return changed;
}

bool ChoiceRow(float y, const char* label, const char* cvar, const std::vector<std::pair<std::string, std::string>>& choices,
               const std::string& hint) {
    RowLayout row = BeginRow(y, label, hint);
    const std::string current = trap::CvarString(cvar);
    int index = 0;
    for (size_t i = 0; i < choices.size(); i++) {
        if (choices[i].first == current) {
            index = static_cast<int>(i);
        }
    }
    const Rect r{theme::kMargin - 40, y, kit::Width() - 2 * theme::kMargin + 40, kRowH};
    int dir = Adjusted(row.id);
    if (!dir && Activated(row.id, r)) {
        dir = 1;
    }
    bool changed = false;
    if (dir && !choices.empty()) {
        index = (index + dir + static_cast<int>(choices.size())) % static_cast<int>(choices.size());
        trap::CvarSet(cvar, choices[index].first.c_str());
        changed = true;
    }
    const kit::Font& mono = kit::GetFonts().mono;
    const Color arrow = row.focused ? theme::kCreamSoft : theme::kCreamFaint;
    mono.Draw(row.controlX, row.baseline, 24, "←", arrow);
    mono.Draw(row.controlX + 44, row.baseline, 24, choices.empty() ? "—" : kit::Upper(choices[index].second),
              theme::kCream, 0.08f);
    const float w = choices.empty() ? 20 : mono.Measure(24, kit::Upper(choices[index].second), 0.08f);
    mono.Draw(row.controlX + 44 + w + 24, row.baseline, 24, "→", arrow);
    return changed;
}

bool NumberRow(float y, const char* label, float value, float lo, float hi, const char* format, float& out,
               const std::string& hint) {
    RowLayout row = BeginRow(y, label, hint);
    const Rect r{theme::kMargin - 40, y, kit::Width() - 2 * theme::kMargin + 40, kRowH};
    const kit::Font& mono = kit::GetFonts().mono;
    bool changed = false;

    if (ui.editing == row.id) {
        if (g_editCommit) {
            const float v = static_cast<float>(std::atof(ui.editBuffer.c_str()));
            ui.editing = -1;
            if (!ui.editBuffer.empty() && v >= lo && v <= hi) {
                out = v;
                changed = true;
            }
        } else if (g_editCancel) {
            ui.editing = -1;
        }
    } else if (Activated(row.id, r)) {
        ui.editing = row.id;
        ui.editBuffer.clear();
    } else {
        Adjusted(row.id);  // Left/Right do nothing here (and don't switch tabs)
    }

    if (ui.editing == row.id) {
        const float w = mono.Draw(row.controlX, row.baseline, 24, ui.editBuffer, theme::kCream);
        if ((ui.realtime / 500) % 2 == 0) {
            kit::Fill(row.controlX + w + 4, row.baseline - 22, 2, 26, kEmber);
        }
        Label(row.controlX + 260, row.baseline - 2, "ENTER  SAVE   ·   ESC  CANCEL", theme::kCreamFaint);
    } else {
        mono.Draw(row.controlX, row.baseline, 24, FormatValue(format, value), theme::kCream);
        if (row.focused) {
            Label(row.controlX + 260, row.baseline - 2, "ENTER  TYPE A VALUE", theme::kCreamFaint);
        }
    }
    return changed;
}

}  // namespace ui
