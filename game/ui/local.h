// UI module state, shared by the screens.
//
// The UI is immediate-mode: every frame each screen lays itself out and draws,
// and widgets read the input gathered since the last frame (widgets.cpp).
// Keyboard and mouse both work everywhere: arrows move the focus, Enter
// activates, Left/Right adjust, Esc goes back.
//
// This file is part of Skaar. GPLv2 or later.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../shared/arenalog.h"
#include "../shared/charts.h"
#include "../shared/draw2d.h"
#include "../shared/insights.h"
#include "../shared/runlog.h"
#include "../shared/scenario.h"
#include "syscalls.h"

namespace ui {

namespace kit = skaar::ui;
namespace theme = skaar::ui::theme;
namespace chart = skaar::ui::chart;
using chart::Rect;
using kit::Align;
using kit::Color;

constexpr Color kEmber = theme::kEmber;

// Type scale for the module, in canvas units (1080 tall). Nothing smaller than
// 15: the old menus were unreadable, and these must not be.
namespace type {
constexpr float kHero = 112;      // display: the home statement
constexpr float kTitle = 72;      // display: screen and item titles
constexpr float kBigNumber = 96;  // display: headline numbers
constexpr float kNav = 21;        // text, uppercase, tracked
constexpr float kItem = 38;       // text, uppercase: home and pause menus
constexpr float kRow = 26;        // text: list rows
constexpr float kBody = 24;       // text: sentences
constexpr float kValue = 28;      // mono: numbers in cells and tables
constexpr float kLabel = 15;      // mono, uppercase: labels, eyebrows, hints
}  // namespace type

enum class Screen { kNone, kHome, kPlay, kInsights, kSettings, kInGame, kEditor, kFirstRun };
enum class InsightsTab { kOverview, kScenarios, kRun, kBenchmarks, kArena };
enum class SettingsTab { kAim, kCrosshair, kControls, kGame, kVideo, kAudio };

// Input gathered from key/mouse events between frames
struct Input {
    float mx = 0, my = 0;
    bool mouseMoved = false;
    bool mouseUsed = false;   // the pointer has moved since the screen opened
    bool click = false;       // mouse1 went down
    bool mouseDown = false;   // mouse1 is held (slider drags)
    int wheel = 0;            // + up
    std::vector<int> keys;    // key presses (not chars)
    std::string chars;        // typed characters
};

// Everything read from disk; refreshed whenever a screen opens
struct Data {
    std::vector<skaar::Scenario> scenarios;
    std::vector<std::pair<std::string, std::string>> scenarioErrors;  // id, error
    std::vector<skaar::Benchmark> benchmarks;
    std::vector<std::pair<std::string, std::string>> benchmarkErrors;  // id, error
    int historySkipped = 0;   // unreadable lines in stats/history.jsonl (kept on disk, left out here)
    std::vector<skaar::RunSummary> history;
    std::vector<skaar::ArenaSession> arena;  // stats/arena.jsonl
    std::map<std::string, skaar::insights::ScenarioStats> perScenario;
    skaar::insights::Overview overview;
    std::vector<skaar::insights::BenchmarkProgress> progress;
    skaar::RunTime now;

    const skaar::Scenario* FindScenario(const std::string& id) const;
    std::string NameOf(const std::string& scenarioId) const;
};

struct State {
    glconfig_t glconfig{};
    Screen screen = Screen::kNone;
    bool fromGame = false;    // inner screens opened from the pause menu return there
    int realtime = 0;
    int openedAt = 0;
    std::string version;
    std::string lastError;

    // focus (widgets.cpp)
    int focus = 0;
    int focusCount = 0;
    int prevFocusCount = 0;
    bool activate = false;    // Enter this frame
    int adjust = 0;           // Left/Right this frame
    bool back = false;        // Esc this frame
    int editing = -1;         // focus index of the number field being typed into
    std::string editBuffer;
    std::string capturing;    // Settings › Controls: command waiting for a key ("" = not capturing)

    // screens
    std::string category = "all";      // play filter
    std::string selectedScenario;      // play + insights scenarios
    InsightsTab insightsTab = InsightsTab::kOverview;
    SettingsTab settingsTab = SettingsTab::kAim;
    std::string selectedRun;           // file of the run shown in Insights › Run
    std::string pinnedRun;             // compared against in Insights › Run (else the scenario's best)
    bool drawing = false;              // a screen is drawing: Open() has to wait for the frame to end
    bool openRunAfterFrame = false;    // ShowRun() was asked for from inside that draw
    std::map<std::string, skaar::RunRecord> records;  // loaded run files, a few at a time
    std::string runError;
    std::map<std::string, float> scroll;  // per list
    std::optional<skaar::Scenario> draft;  // the editor's working copy

    qhandle_t mark = 0, pointer = 0;
    sfxHandle_t move = 0, confirm = 0;  // focus moved / something activated
};

extern State ui;
extern Input in;
extern Data data;

// main.cpp
void Exec(const char* text);
void Open(Screen screen);
void Close();
bool InGame();
void StartScenario(const std::string& id);
void ShowRun(const std::string& file);

// data.cpp
void LoadData();
const skaar::RunRecord* LoadRunRecord(const std::string& file);

// widgets.cpp
void BeginFrame();
void EndFrame();
void Marker(float x, float baseline, float cap);  // the sliding ember bar points here this frame
void DrawMarker();
void ResetMarker();                              // snap (no slide) on the next frame
int TakeAdjust();
void SetRowHeight(float h);                      // settings-style rows (default 64)                                // Left/Right nobody used: screens cycle their tabs with it
int Focusable(const Rect& r);          // registers a focus stop; returns its id
bool Focused(int id);
bool Activated(int id, const Rect& r);  // click on r, or Enter while focused
int Adjusted(int id);                   // -1 / +1 when focused and Left/Right pressed
bool Hovered(const Rect& r);
float Scroll(const std::string& key, const Rect& view, float contentHeight, float focusTop, float focusBottom,
             float rowHeight);

void Label(float x, float baseline, const std::string& text, Color color = theme::kCreamFaint,
           Align align = Align::kLeft);
std::vector<std::string> Wrap(const kit::Font& font, float size, const std::string& text, float width);
float Paragraph(float x, float y, float width, const std::string& text, float size = type::kBody,
                Color color = theme::kCreamSoft, float lineHeight = 1.45f);
void Header(const std::string& eyebrowLeft);
void Footer(const std::string& hints);
void Pointer();
bool Tab(float x, float baseline, const std::string& label, bool active, float* outWidth = nullptr);
bool ActionButton(const Rect& r, const std::string& label, const std::string& hint = "");
void Cell(const Rect& r, const std::string& label, const std::string& value, Color valueColor = theme::kCream,
          float valueSize = type::kValue);
void CellGrid(float x, float y, float cellW, float cellH, int cols, int count);
std::string FormatDuration(float seconds);   // 1:05, 1 h 05 m
std::string FormatDate(const skaar::RunTime& t, const skaar::RunTime& now);  // Today 18:30, Oct 21

// Settings rows: label left, control right; return true when changed
bool SliderRow(float y, const char* label, const char* cvar, float lo, float hi, float step, const char* format,
               const std::string& hint = "");
bool ToggleRow(float y, const char* label, const char* cvar, const std::string& hint = "");
bool ChoiceRow(float y, const char* label, const char* cvar, const std::vector<std::pair<std::string, std::string>>& choices,
               const std::string& hint = "");
bool ValueSlider(float y, const char* label, float& value, float lo, float hi, float step, const char* format,
                 const std::string& hint = "");
bool ValueChoice(float y, const char* label, int& index, const std::vector<std::string>& choices,
                 const std::string& hint = "");
bool NumberRow(float y, const char* label, float value, float lo, float hi, const char* format, float& out,
               const std::string& hint = "");

// screens
void DrawHome();
void DrawPlay();
void DrawInsights();
void DrawSettings();
void DrawInGame();
void DrawEditor();
void DrawFirstRun();
bool FirstRunPending();
void EditScenario(const std::string& id);
void DrawConnect(bool overlay);

// shared drawing for insights + play
void ScoreChart(const Rect& r, const std::vector<int>& runs, bool drawAxis = true);

}  // namespace ui
