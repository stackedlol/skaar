// Client game -> engine calls (cgameImport_t in code/cgame/cg_public.h).
//
// This file is part of Skaar. GPLv2 or later.
#pragma once

#include <string>

#include "../shared/q3.h"

extern "C" {
#include "renderercommon/tr_types.h"
#include "cgame/cg_public.h"

// called by the reused bg_*.c code; defined in syscalls.cpp
void trap_Cvar_VariableStringBuffer(const char* name, char* buffer, int size);
void trap_SnapVector(float* v);
}

namespace cg::trap {

void Print(const char* text);
[[noreturn]] void Error(const char* text);
int Milliseconds();
void CvarRegister(vmCvar_t* cvar, const char* name, const char* value, int flags);
void CvarUpdate(vmCvar_t* cvar);
void CvarSet(const char* name, const char* value);
std::string CvarString(const char* name);
int Argc();
std::string Argv(int n);
void AddCommand(const char* name);
void RemoveCommand(const char* name);
void SendClientCommand(const char* text);
void SendConsoleCommand(const char* text);
bool FileRead(const char* path, std::string& out);
bool FileWrite(const char* path, const std::string& data, bool append);
int RealTime(qtime_t* t);

void CM_LoadMap(const char* name);
void CM_BoxTrace(trace_t* results, const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs,
                 clipHandle_t model, int brushmask);
int CM_PointContents(const vec3_t point, clipHandle_t model);
clipHandle_t CM_InlineModel(int index);
void CM_TransformedBoxTrace(trace_t* results, const vec3_t start, const vec3_t end, const vec3_t mins,
                            const vec3_t maxs, clipHandle_t model, int brushmask, const vec3_t origin,
                            const vec3_t angles);

void R_LoadWorldMap(const char* name);
qhandle_t R_RegisterShader(const char* name);
qhandle_t R_RegisterModel(const char* name);
void R_ClearScene();
void R_AddRefEntityToScene(const refEntity_t* ent);
void R_RenderScene(const refdef_t* fd);
void R_SetColor(const float* rgba);
void R_DrawStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t shader);

void GetGlconfig(glconfig_t* config);
void GetGameState(gameState_t* state);
void GetCurrentSnapshotNumber(int* snapshotNumber, int* serverTime);
bool GetSnapshot(int snapshotNumber, snapshot_t* snapshot);
bool GetServerCommand(int serverCommandNumber);
int GetCurrentCmdNumber();
bool GetUserCmd(int cmdNumber, usercmd_t* cmd);
void SetUserCmdValue(int stateValue, float sensitivityScale);
int Key_GetCatcher();

sfxHandle_t S_RegisterSound(const char* name, bool compressed);
void S_StartLocalSound(sfxHandle_t sfx, int channel);

}  // namespace cg::trap
