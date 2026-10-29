// Client game state.
//
// This file is part of Skaar. GPLv2 or later.
#pragma once

#include <array>
#include <string>

#include "syscalls.h"

namespace cg {

struct CEntity {
    entityState_t current;  // from the current snapshot
    entityState_t next;     // from the next snapshot, when interpolating
    bool valid;             // present in the current snapshot
    bool interpolate;       // next is usable for lerping
    vec3_t lerpOrigin;
    vec3_t lerpAngles;
    int previousEvent;      // last entity event fired (snapshot.cpp CheckEvents)
};

struct State {
    int clientNum;
    glconfig_t glconfig;
    gameState_t gameState;
    std::string mapname;
    int levelStartTime;

    // snapshots (snapshot.cpp)
    std::array<snapshot_t, 2> snapshots;
    snapshot_t* snap;      // the one cg.time is past
    snapshot_t* nextSnap;  // the one cg.time is heading to
    int processedSnapshotNum;
    int latestSnapshotNum;
    int latestSnapshotTime;
    int serverCommandSequence;
    bool thisFrameTeleport;
    bool nextFrameTeleport;
    float frameInterpolation;

    // time
    int time;     // server time this frame
    int oldTime;
    int frametime;
    int realTime;

    // prediction (predict.cpp)
    playerState_t predicted;
    bool validPredicted;
    int physicsTime;
    vec3_t predictedError;
    int predictedErrorTime;
    int lastTrainerCmd;  // newest command number handed to trainer::OnCommand
    int lastPlayerEventSeq;   // newest predicted ps event handed to weapons::PlayerEvent
    int lastExternalEventTime;

    // view (view.cpp)
    refdef_t refdef;
    vec3_t viewAngles;

    // hud
    std::string centerPrint;
    int centerPrintTime;
};

struct Cvars {
    vmCvar_t vidSize;  // r_vidSize: changes when the window's drawable size does
    vmCvar_t fov, drawHud, drawCrosshair, crosshairSize, crosshairGap, drawSpeed, errorDecay, noPredict, showMiss,
        pmoveFixed, pmoveMsec;
};

extern State cg;
extern Cvars cvars;
extern std::array<CEntity, MAX_GENTITIES> entities;

const char* ConfigString(int index);
void SyncRuleset();  // main.cpp: mirror the server's movement ruleset

// snapshot.cpp
void ProcessSnapshots();

// predict.cpp
void PredictPlayerState();

// view.cpp
void DrawActiveFrame(int serverTime, bool demoPlayback);

// hud.cpp
void InitHud();
void DrawHud();

}  // namespace cg
