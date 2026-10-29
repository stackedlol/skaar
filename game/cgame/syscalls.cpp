// This file is part of Skaar. GPLv2 or later.
#include "syscalls.h"

#include <cstdlib>

using skaar::PassFloat;

namespace {
intptr_t(QDECL* g_syscall)(intptr_t arg, ...) = nullptr;
}

SKAAR_MODULE_EXPORT void dllEntry(intptr_t(QDECL* syscallptr)(intptr_t arg, ...)) { g_syscall = syscallptr; }

extern "C" void trap_Cvar_VariableStringBuffer(const char* name, char* buffer, int size) {
    g_syscall(CG_CVAR_VARIABLESTRINGBUFFER, name, buffer, size);
}

extern "C" void trap_SnapVector(float* v) { g_syscall(CG_SNAPVECTOR, v); }

namespace cg::trap {

void Print(const char* text) { g_syscall(CG_PRINT, text); }

void Error(const char* text) {
    g_syscall(CG_ERROR, text);
    std::abort();  // CG_ERROR longjmps out of the module; never reached
}

int Milliseconds() { return static_cast<int>(g_syscall(CG_MILLISECONDS)); }

void CvarRegister(vmCvar_t* cvar, const char* name, const char* value, int flags) {
    g_syscall(CG_CVAR_REGISTER, cvar, name, value, flags);
}

void CvarUpdate(vmCvar_t* cvar) { g_syscall(CG_CVAR_UPDATE, cvar); }
void CvarSet(const char* name, const char* value) { g_syscall(CG_CVAR_SET, name, value); }

std::string CvarString(const char* name) {
    char buf[MAX_CVAR_VALUE_STRING];
    trap_Cvar_VariableStringBuffer(name, buf, sizeof(buf));
    return buf;
}

int Argc() { return static_cast<int>(g_syscall(CG_ARGC)); }

std::string Argv(int n) {
    char buf[MAX_STRING_CHARS];
    g_syscall(CG_ARGV, n, buf, sizeof(buf));
    return buf;
}

void AddCommand(const char* name) { g_syscall(CG_ADDCOMMAND, name); }
void RemoveCommand(const char* name) { g_syscall(CG_REMOVECOMMAND, name); }
void SendClientCommand(const char* text) { g_syscall(CG_SENDCLIENTCOMMAND, text); }
void SendConsoleCommand(const char* text) { g_syscall(CG_SENDCONSOLECOMMAND, text); }

bool FileRead(const char* path, std::string& out) {
    fileHandle_t f;
    const int len = static_cast<int>(g_syscall(CG_FS_FOPENFILE, path, &f, FS_READ));
    if (len < 0 || !f) {
        return false;
    }
    out.resize(len);
    g_syscall(CG_FS_READ, out.data(), len, f);
    g_syscall(CG_FS_FCLOSEFILE, f);
    return true;
}

bool FileWrite(const char* path, const std::string& data, bool append) {
    fileHandle_t f;
    g_syscall(CG_FS_FOPENFILE, path, &f, append ? FS_APPEND : FS_WRITE);
    if (!f) {
        return false;
    }
    g_syscall(CG_FS_WRITE, data.data(), static_cast<int>(data.size()), f);
    g_syscall(CG_FS_FCLOSEFILE, f);
    return true;
}

int RealTime(qtime_t* t) { return static_cast<int>(g_syscall(CG_REAL_TIME, t)); }

void CM_LoadMap(const char* name) { g_syscall(CG_CM_LOADMAP, name); }

void CM_BoxTrace(trace_t* results, const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs,
                 clipHandle_t model, int brushmask) {
    g_syscall(CG_CM_BOXTRACE, results, start, end, mins, maxs, model, brushmask);
}

int CM_PointContents(const vec3_t point, clipHandle_t model) {
    return static_cast<int>(g_syscall(CG_CM_POINTCONTENTS, point, model));
}

clipHandle_t CM_InlineModel(int index) { return static_cast<clipHandle_t>(g_syscall(CG_CM_INLINEMODEL, index)); }

void CM_TransformedBoxTrace(trace_t* results, const vec3_t start, const vec3_t end, const vec3_t mins,
                            const vec3_t maxs, clipHandle_t model, int brushmask, const vec3_t origin,
                            const vec3_t angles) {
    g_syscall(CG_CM_TRANSFORMEDBOXTRACE, results, start, end, mins, maxs, model, brushmask, origin, angles);
}

void R_LoadWorldMap(const char* name) { g_syscall(CG_R_LOADWORLDMAP, name); }
qhandle_t R_RegisterShader(const char* name) { return static_cast<qhandle_t>(g_syscall(CG_R_REGISTERSHADER, name)); }
qhandle_t R_RegisterModel(const char* name) { return static_cast<qhandle_t>(g_syscall(CG_R_REGISTERMODEL, name)); }
void R_ClearScene() { g_syscall(CG_R_CLEARSCENE); }
void R_AddRefEntityToScene(const refEntity_t* ent) { g_syscall(CG_R_ADDREFENTITYTOSCENE, ent); }
void R_RenderScene(const refdef_t* fd) { g_syscall(CG_R_RENDERSCENE, fd); }
void R_SetColor(const float* rgba) { g_syscall(CG_R_SETCOLOR, rgba); }

void R_DrawStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t shader) {
    g_syscall(CG_R_DRAWSTRETCHPIC, PassFloat(x), PassFloat(y), PassFloat(w), PassFloat(h), PassFloat(s1),
              PassFloat(t1), PassFloat(s2), PassFloat(t2), shader);
}

void GetGlconfig(glconfig_t* config) { g_syscall(CG_GETGLCONFIG, config); }
void GetGameState(gameState_t* state) { g_syscall(CG_GETGAMESTATE, state); }

void GetCurrentSnapshotNumber(int* snapshotNumber, int* serverTime) {
    g_syscall(CG_GETCURRENTSNAPSHOTNUMBER, snapshotNumber, serverTime);
}

bool GetSnapshot(int snapshotNumber, snapshot_t* snapshot) {
    return g_syscall(CG_GETSNAPSHOT, snapshotNumber, snapshot) != 0;
}

bool GetServerCommand(int serverCommandNumber) { return g_syscall(CG_GETSERVERCOMMAND, serverCommandNumber) != 0; }
int GetCurrentCmdNumber() { return static_cast<int>(g_syscall(CG_GETCURRENTCMDNUMBER)); }
bool GetUserCmd(int cmdNumber, usercmd_t* cmd) { return g_syscall(CG_GETUSERCMD, cmdNumber, cmd) != 0; }

void SetUserCmdValue(int stateValue, float sensitivityScale) {
    g_syscall(CG_SETUSERCMDVALUE, stateValue, PassFloat(sensitivityScale));
}

int Key_GetCatcher() { return static_cast<int>(g_syscall(CG_KEY_GETCATCHER)); }

sfxHandle_t S_RegisterSound(const char* name, bool compressed) {
    return static_cast<sfxHandle_t>(g_syscall(CG_S_REGISTERSOUND, name, compressed ? qtrue : qfalse));
}

void S_StartLocalSound(sfxHandle_t sfx, int channel) { g_syscall(CG_S_STARTLOCALSOUND, sfx, channel); }

}  // namespace cg::trap
