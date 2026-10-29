// UI module: vmMain dispatch, screen switching, input routing.
// Screens live in screen_*.cpp; widgets in widgets.cpp; disk data in data.cpp.
// Style: VØLUND stage with one ember accent and a faint arena grid.
//
// This file is part of Skaar. GPLv2 or later.
#include <cmath>
#include <cstdarg>
#include <exception>

#include "../shared/move_ruleset.h"
#include "local.h"

namespace ui {

State ui;
Input in;
Data data;

void Exec(const char* text) { trap::CmdExecuteText(EXEC_APPEND, text); }

bool InGame() {
    uiClientState_t cs;
    trap::GetClientState(&cs);
    return cs.connState == CA_ACTIVE;
}

void Close() {
    trap::Key_SetCatcher(trap::Key_GetCatcher() & ~KEYCATCH_UI);
    trap::Key_ClearStates();
    ui.screen = Screen::kNone;
    ui.editing = -1;
}

void Open(Screen screen) {
    if (ui.screen == Screen::kNone || ui.screen == Screen::kHome || ui.screen == Screen::kInGame) {
        ui.fromGame = InGame();
    }
    ui.screen = screen;
    ui.focus = 0;
    ResetMarker();
    ui.editing = -1;
    ui.openedAt = ui.realtime;
    in.mouseUsed = false;
    in.mx = kit::Width() / 2;
    in.my = kit::Height() / 2;
    in.keys.clear();
    trap::Key_SetCatcher(KEYCATCH_UI);
    LoadData();

    // surface (once) the error that dropped us back here
    const std::string err = trap::CvarString("com_errorMessage");
    if (!err.empty()) {
        ui.lastError = err;
        trap::CvarSet("com_errorMessage", "");
    }
}

// Go back one level: inner screens → home (or the pause menu in game); pause → game
void Back() {
    switch (ui.screen) {
        case Screen::kInGame:
            Close();
            break;
        case Screen::kHome:
        case Screen::kFirstRun:
            if (InGame()) {
                Open(Screen::kInGame);
            }
            break;
        default:
            if (ui.screen == Screen::kEditor) {
                ui.draft.reset();
                Open(Screen::kPlay);
                return;
            }
            if (ui.screen == Screen::kInsights && ui.insightsTab == InsightsTab::kRun) {
                ui.insightsTab = InsightsTab::kOverview;
                ui.focus = 0;
                return;
            }
            Open(ui.fromGame && InGame() ? Screen::kInGame : Screen::kHome);
            break;
    }
}

void StartScenario(const std::string& id) {
    const skaar::Scenario* s = data.FindScenario(id);
    if (!s) {
        return;
    }
    trap::CvarSet("sk_scenario", id.c_str());
    // movement physics: the server switches when g_movement changes (game/shared/move_ruleset.h)
    std::string ruleset = s->ruleset;
    if (skaar::move::FindRuleset(ruleset) < 0 && !trap::FileRead(skaar::Va("rulesets/%s.json", ruleset.c_str()), ruleset)) {
        Com_Printf(S_COLOR_YELLOW "WARNING: %s: unknown movement ruleset '%s', using vq3\n", id.c_str(), s->ruleset.c_str());
        ruleset = "vq3";
    } else {
        ruleset = s->ruleset;
    }
    trap::CvarSet("g_movement", ruleset.c_str());
    const std::string map = Info_ValueForKey(trap::GetConfigString(CS_SERVERINFO).c_str(), "mapname");
    if (InGame() && map == s->map) {
        Exec(skaar::Va("sk_start %s\n", id.c_str()));
    } else {
        Exec(skaar::Va("map %s\n", s->map.c_str()));
    }
    Close();
}

void ShowRun(const std::string& file) {
    ui.selectedRun = file;
    ui.insightsTab = InsightsTab::kRun;
    if (ui.screen != Screen::kInsights) {
        Open(Screen::kInsights);
        ui.insightsTab = InsightsTab::kRun;
    }
    ui.focus = 0;
}

namespace {

// ---- input ------------------------------------------------------------------------

void KeyEvent(int key, bool down) {
    if (ui.screen == Screen::kNone) {
        return;
    }
    // Settings › Controls is waiting for a key: the next press (any button) binds it
    if (!ui.capturing.empty()) {
        if (!down || (key & K_CHAR_FLAG)) {
            return;
        }
        if (key != K_ESCAPE) {
            for (int k = 0; k < MAX_KEYS; k++) {
                if (!Q_stricmp(trap::KeyBinding(k).c_str(), ui.capturing.c_str())) {
                    trap::SetBinding(k, "");
                }
            }
            trap::SetBinding(key, ui.capturing.c_str());
        }
        ui.capturing.clear();
        return;
    }
    if (key & K_CHAR_FLAG) {
        if (down) {
            in.chars += static_cast<char>(key & ~K_CHAR_FLAG);
        }
        return;
    }
    if (key == K_MOUSE1) {
        in.mouseDown = down;
        if (down) {
            in.click = true;
        }
        return;
    }
    if (!down) {
        return;
    }
    // WASD moves like the arrows (letters only matter while typing a number)
    if (ui.editing < 0) {
        switch (key) {
            case 'w':
                key = K_UPARROW;
                break;
            case 's':
                key = K_DOWNARROW;
                break;
            case 'a':
                key = K_LEFTARROW;
                break;
            case 'd':
                key = K_RIGHTARROW;
                break;
            default:
                break;
        }
    }
    if (key == K_MWHEELUP) {
        in.wheel++;
    } else if (key == K_MWHEELDOWN) {
        in.wheel--;
    } else if (key == K_MOUSE2 && ui.editing < 0) {
        in.keys.push_back(K_ESCAPE);  // right click goes back
    } else {
        in.keys.push_back(key);
    }
}

void MouseEvent(int dx, int dy) {
    in.mouseUsed = true;
    in.mouseMoved = true;
    in.systemCursor = false;
    in.mx = std::fmin(std::fmax(0.0f, in.mx + dx / kit::Scale()), kit::Width());
    in.my = std::fmin(std::fmax(0.0f, in.my + dy / kit::Scale()), kit::Height());
}

// Windowed menus leave the mouse free: x, y is the system cursor in pixels
void MousePosition(int x, int y) {
    in.mouseUsed = true;
    in.mouseMoved = true;
    in.systemCursor = true;
    in.mx = std::fmin(std::fmax(0.0f, x / kit::Scale()), kit::Width());
    in.my = std::fmin(std::fmax(0.0f, y / kit::Scale()), kit::Height());
}

// ---- lifecycle -----------------------------------------------------------------

bool ReadFileForKit(const char* path, std::string& out) { return trap::FileRead(path, out); }

void Init() {
    ui = State{};
    in = Input{};
    trap::GetGlconfig(&ui.glconfig);
    trap::CvarRegister(&ui.vidSize, "r_vidSize", "", CVAR_ROM);
    kit::Backend backend{};
    backend.registerShader = trap::R_RegisterShaderNoMip;  // scripts/skaar-ui.shader adds mipmaps
    backend.setColor = trap::R_SetColor;
    backend.drawStretchPic = trap::R_DrawStretchPic;
    backend.readFile = ReadFileForKit;
    if (!kit::Init(backend, ui.glconfig.vidWidth, ui.glconfig.vidHeight)) {
        Com_Printf(S_COLOR_YELLOW "WARNING: ui: fonts missing (run tools/gen_assets.py)\n");
    }
    chart::Init(backend);
    ui.mark = trap::R_RegisterShaderNoMip("gfx/skaar/mark");
    ui.pointer = trap::R_RegisterShaderNoMip("gfx/skaar/cursor");
    ui.move = trap::S_RegisterSound("sound/ui/move.wav");
    ui.confirm = trap::S_RegisterSound("sound/ui/confirm.wav");
    // menu-owned settings that must survive restarts
    static vmCvar_t archived[6];
    const char* names[][2] = {{"sk_firstRunDone", "0"}, {"sk_convertYaw", "0.022"}, {"sk_convertSens", "0"},
                              {"sk_arenaMovement", "cpm"}, {"sk_roamMovement", "cpm"}, {"sk_dpi", "800"}};
    for (int i = 0; i < 6; i++) {
        trap::CvarRegister(&archived[i], names[i][0], names[i][1], CVAR_ARCHIVE);
    }
    ui.version = trap::CvarString("version");
    if (const size_t space = ui.version.find(' '); space != std::string::npos) {
        ui.version = "v" + ui.version.substr(space + 1, ui.version.find(' ', space + 1) - space - 1);
    }
    Com_DPrintf("ui: init %dx%d\n", ui.glconfig.vidWidth, ui.glconfig.vidHeight);
}

void Refresh(int realtime) {
    ui.realtime = realtime;
    // window resized or fullscreen toggled: no vid_restart, just the new size
    const int vidSizeCount = ui.vidSize.modificationCount;
    trap::CvarUpdate(&ui.vidSize);
    if (ui.vidSize.modificationCount != vidSizeCount) {
        trap::GetGlconfig(&ui.glconfig);
        kit::Resize(ui.glconfig.vidWidth, ui.glconfig.vidHeight);
        in.mx = std::fmin(in.mx, kit::Width());
    }
    if (!(trap::Key_GetCatcher() & KEYCATCH_UI) || ui.screen == Screen::kNone) {
        return;
    }
    BeginFrame();
    switch (ui.screen) {
        case Screen::kHome:
            DrawHome();
            break;
        case Screen::kPlay:
            DrawPlay();
            break;
        case Screen::kInsights:
            DrawInsights();
            break;
        case Screen::kSettings:
            DrawSettings();
            break;
        case Screen::kInGame:
            DrawInGame();
            break;
        case Screen::kEditor:
            DrawEditor();
            break;
        case Screen::kFirstRun:
            DrawFirstRun();
            break;
        case Screen::kNone:
            break;
    }
    if (ui.back) {
        Back();
    }
    DrawMarker();
    Pointer();
    EndFrame();
}

void SetActiveMenu(int menu) {
    switch (menu) {
        case UIMENU_NONE:
            Close();
            break;
        case UIMENU_INGAME:
            Open(Screen::kInGame);
            break;
        default:  // UIMENU_MAIN and the legacy ones (cd key, team, postgame)
            Open(FirstRunPending() && !InGame() ? Screen::kFirstRun : Screen::kHome);
            break;
    }
}

// "skaar_ui <home|play|insights|scenarios|benchmarks|settings|run <file>>": the cgame and binds open screens with it
bool ConsoleCommand() {
    if (Q_stricmp(trap::Argv(0).c_str(), "skaar_ui")) {
        return false;
    }
    const std::string what = trap::Argv(1);
    if (what == "run" && trap::Argc() > 2) {
        ShowRun(trap::Argv(2));
    } else if (what == "edit" && trap::Argc() > 2) {
        LoadData();
        EditScenario(trap::Argv(2));
    } else if (what == "insights" || what == "benchmarks" || what == "scenarios" || what == "arena") {
        Open(Screen::kInsights);
        ui.insightsTab = what == "benchmarks"  ? InsightsTab::kBenchmarks
                         : what == "scenarios" ? InsightsTab::kScenarios
                         : what == "arena"     ? InsightsTab::kArena
                                               : InsightsTab::kOverview;
    } else if (what == "firstrun") {
        Open(Screen::kFirstRun);
    } else if (what == "play") {
        Open(Screen::kPlay);
    } else if (what == "settings" || what == "controls") {
        Open(Screen::kSettings);
        ui.settingsTab = what == "controls" ? SettingsTab::kControls : SettingsTab::kAim;
    } else {
        Open(InGame() ? Screen::kInGame : Screen::kHome);
    }
    return true;
}

intptr_t Dispatch(int command, int arg0, int arg1) {
    switch (command) {
        case UI_GETAPIVERSION:
            return UI_API_VERSION;
        case UI_INIT:
            Init();
            return 0;
        case UI_SHUTDOWN:
            return 0;
        case UI_KEY_EVENT:
            KeyEvent(arg0, arg1 != 0);
            return 0;
        case UI_MOUSE_EVENT:
            MouseEvent(arg0, arg1);
            return 0;
        case UI_MOUSE_POSITION:
            MousePosition(arg0, arg1);
            return 0;
        case UI_REFRESH:
            Refresh(arg0);
            return 0;
        case UI_IS_FULLSCREEN:
            return ui.screen != Screen::kNone && ui.screen != Screen::kInGame && !InGame();
        case UI_SET_ACTIVE_MENU:
            SetActiveMenu(arg0);
            return 0;
        case UI_CONSOLE_COMMAND:
            return ConsoleCommand();
        case UI_DRAW_CONNECT_SCREEN:
            DrawConnect(arg0 != 0);
            return 0;
        case UI_HASUNIQUECDKEY:
            return 0;
        default:
            Com_Printf(S_COLOR_YELLOW "WARNING: ui: unknown vmMain command %i\n", command);
            return -1;
    }
}

}  // namespace
}  // namespace ui

SKAAR_MODULE_EXPORT intptr_t vmMain(int command, int arg0, int arg1, int, int, int, int, int, int, int, int, int,
                                    int) {
    try {
        return ui::Dispatch(command, arg0, arg1);
    } catch (const std::exception& e) {
        Com_Error(ERR_DROP, "ui: %s", e.what());
    }
}

extern "C" void QDECL Com_Printf(const char* fmt, ...) {
    va_list ap;
    char text[skaar::kMaxPrint];
    va_start(ap, fmt);
    Q_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    ui::trap::Print(text);
}

extern "C" void QDECL Com_DPrintf(const char* fmt, ...) {
    if (ui::trap::CvarString("developer") == "0") {
        return;
    }
    va_list ap;
    char text[skaar::kMaxPrint];
    va_start(ap, fmt);
    Q_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    ui::trap::Print(text);
}

extern "C" void QDECL Com_Error(int level, const char* fmt, ...) {
    (void)level;
    va_list ap;
    char text[skaar::kMaxPrint];
    va_start(ap, fmt);
    Q_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    ui::trap::Error(text);
}
