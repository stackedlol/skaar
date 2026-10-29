// Client game module: vmMain dispatch, init (map, media), console commands.
//
// This file is part of Skaar. GPLv2 or later.
#include <cstdarg>
#include <exception>

#include "../shared/draw2d.h"
#include "../shared/reticle.h"
#include "local.h"
#include "shared/move_ruleset.h"
#include "trainer.h"
#include "weapons.h"

namespace cg {

State cg;
Cvars cvars;
std::array<CEntity, MAX_GENTITIES> entities;

const char* ConfigString(int index) {
    if (index < 0 || index >= MAX_CONFIGSTRINGS) {
        Com_Error(ERR_DROP, "cgame: configstring %d out of range", index);
    }
    return cg.gameState.stringData + cg.gameState.stringOffsets[index];
}

// Mirrors the server's movement ruleset, so prediction matches (move_ruleset.h).
void SyncRuleset() {
    std::string error;
    if (!skaar::move::DecodeRuleset(ConfigString(skaar::move::kConfigstringRuleset), error)) {
        Com_Printf(S_COLOR_YELLOW "WARNING: movement ruleset from the server: %s\n", error.c_str());
    }
}

namespace {

void RegisterCvars() {
    trap::CvarRegister(&cvars.vidSize, "r_vidSize", "", CVAR_ROM);
    trap::CvarRegister(&cvars.fov, "cg_fov", "103", CVAR_ARCHIVE);
    trap::CvarRegister(&cvars.drawHud, "cg_drawHud", "1", CVAR_ARCHIVE);
    trap::CvarRegister(&cvars.drawCrosshair, "cg_drawCrosshair", "1", CVAR_ARCHIVE);
    // defaults shared with the trainer and the settings preview (game/shared/reticle.h)
    trap::CvarRegister(&cvars.crosshairSize, "cg_crosshairSize", skaar::ui::reticle_defaults::kSize, CVAR_ARCHIVE);
    trap::CvarRegister(&cvars.crosshairGap, "cg_crosshairGap", skaar::ui::reticle_defaults::kGap, CVAR_ARCHIVE);
    trap::CvarRegister(&cvars.drawSpeed, "cg_drawSpeed", "1", CVAR_ARCHIVE);
    trap::CvarRegister(&cvars.errorDecay, "cg_errorDecay", "100", 0);
    trap::CvarRegister(&cvars.noPredict, "cg_noPredict", "0", 0);
    trap::CvarRegister(&cvars.showMiss, "cg_showMiss", "0", 0);
    // set by the server (SYSTEMINFO) so prediction matches it
    trap::CvarRegister(&cvars.pmoveFixed, "pmove_fixed", "1", CVAR_SYSTEMINFO);
    trap::CvarRegister(&cvars.pmoveMsec, "pmove_msec", "8", CVAR_SYSTEMINFO);
}

void UpdateCvars() {
    // window resized or fullscreen toggled: no vid_restart, just the new size
    const int vidSizeCount = cvars.vidSize.modificationCount;
    trap::CvarUpdate(&cvars.vidSize);
    if (cvars.vidSize.modificationCount != vidSizeCount) {
        trap::GetGlconfig(&cg.glconfig);
        skaar::ui::Resize(cg.glconfig.vidWidth, cg.glconfig.vidHeight);
    }
    for (vmCvar_t* c : {&cvars.fov, &cvars.drawHud, &cvars.drawCrosshair, &cvars.crosshairSize, &cvars.crosshairGap,
                        &cvars.drawSpeed, &cvars.errorDecay, &cvars.noPredict, &cvars.showMiss, &cvars.pmoveFixed,
                        &cvars.pmoveMsec}) {
        trap::CvarUpdate(c);
    }
}

void Init(int serverMessageNum, int serverCommandSequence, int clientNum) {
    cg = State{};
    entities = {};
    cg.clientNum = clientNum;
    cg.processedSnapshotNum = serverMessageNum;
    cg.serverCommandSequence = serverCommandSequence;

    RegisterCvars();
    trap::GetGlconfig(&cg.glconfig);
    trap::GetGameState(&cg.gameState);

    if (Q_stricmp(ConfigString(CS_GAME_VERSION), GAME_VERSION)) {
        Com_Error(ERR_DROP, "cgame: server game version '%s', client expects '%s'", ConfigString(CS_GAME_VERSION),
                  GAME_VERSION);
    }
    cg.levelStartTime = atoi(ConfigString(CS_LEVEL_START_TIME));
    cg.mapname = Info_ValueForKey(ConfigString(CS_SERVERINFO), "mapname");
    SyncRuleset();

    // the world: collision for prediction, geometry for the renderer
    const std::string bsp = "maps/" + cg.mapname + ".bsp";
    trap::CM_LoadMap(bsp.c_str());
    trap::R_LoadWorldMap(bsp.c_str());

    InitHud();
    cg.lastTrainerCmd = trap::GetCurrentCmdNumber();  // only commands from now on
    trainer::Init();
    weapons::Init();

    // commands the server handles; registering them makes them complete in the console
    for (const char* cmd : {"noclip", "kill", "respawn"}) {
        trap::AddCommand(cmd);
    }

    Com_DPrintf("cgame: init %s, client %d, fov %d, %dx%d\n", cg.mapname.c_str(), clientNum, cvars.fov.integer,
                cg.glconfig.vidWidth, cg.glconfig.vidHeight);
}

void Shutdown() {
    weapons::Shutdown();
    trainer::Shutdown();
    for (const char* cmd : {"noclip", "kill", "respawn"}) {
        trap::RemoveCommand(cmd);
    }
    Com_DPrintf("cgame: shutdown\n");
}

// Console commands the engine offers us first; true if handled
bool ConsoleCommand() {
    const std::string cmd = trap::Argv(0);
    if (trainer::ConsoleCommand(cmd.c_str()) || weapons::ConsoleCommand(cmd.c_str())) {
        return true;
    }
    if (!Q_stricmp(cmd.c_str(), "viewpos")) {
        Com_Printf("(%.0f %.0f %.0f) yaw %.0f pitch %.0f\n", cg.refdef.vieworg[0], cg.refdef.vieworg[1],
                   cg.refdef.vieworg[2], cg.viewAngles[YAW], cg.viewAngles[PITCH]);
        return true;
    }
    return false;  // the engine forwards it to the server (noclip, kill...)
}

intptr_t Dispatch(int command, int arg0, int arg1, int arg2) {
    switch (command) {
        case CG_INIT:
            Init(arg0, arg1, arg2);
            return 0;
        case CG_SHUTDOWN:
            Shutdown();
            return 0;
        case CG_CONSOLE_COMMAND:
            return ConsoleCommand();
        case CG_DRAW_ACTIVE_FRAME:
            UpdateCvars();
            DrawActiveFrame(arg0, arg2 != 0);
            return 0;
        case CG_CROSSHAIR_PLAYER:
            return -1;
        case CG_LAST_ATTACKER:
            return -1;
        case CG_KEY_EVENT:
        case CG_MOUSE_EVENT:
        case CG_EVENT_HANDLING:
            return 0;
        default:
            Com_Printf(S_COLOR_YELLOW "WARNING: cgame: unknown vmMain command %i\n", command);
            return -1;
    }
}

}  // namespace
}  // namespace cg

SKAAR_MODULE_EXPORT intptr_t vmMain(int command, int arg0, int arg1, int arg2, int, int, int, int, int, int, int,
                                    int, int) {
    try {
        return cg::Dispatch(command, arg0, arg1, arg2);
    } catch (const std::exception& e) {
        Com_Error(ERR_DROP, "cgame: %s", e.what());
    }
}

extern "C" void QDECL Com_Printf(const char* fmt, ...) {
    va_list ap;
    char text[skaar::kMaxPrint];
    va_start(ap, fmt);
    Q_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    cg::trap::Print(text);
}

extern "C" void QDECL Com_DPrintf(const char* fmt, ...) {
    if (cg::trap::CvarString("developer") == "0") {
        return;
    }
    va_list ap;
    char text[skaar::kMaxPrint];
    va_start(ap, fmt);
    Q_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    cg::trap::Print(text);
}

extern "C" void QDECL Com_Error(int level, const char* fmt, ...) {
    (void)level;
    va_list ap;
    char text[skaar::kMaxPrint];
    va_start(ap, fmt);
    Q_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    cg::trap::Error(text);
}
