// This file is part of Skaar. GPLv2 or later.
#include "syscalls.h"

#include <cstdlib>
#include <string>

namespace {
intptr_t(QDECL* g_syscall)(intptr_t arg, ...) = nullptr;
}

SKAAR_MODULE_EXPORT void dllEntry(intptr_t(QDECL* syscallptr)(intptr_t arg, ...)) { g_syscall = syscallptr; }

// bg_misc.c (C) calls this directly
extern "C" void trap_Cvar_VariableStringBuffer(const char* name, char* buffer, int size) {
    g_syscall(G_CVAR_VARIABLE_STRING_BUFFER, name, buffer, size);
}

extern "C" void trap_SnapVector(float* v) { g_syscall(G_SNAPVECTOR, v); }

namespace sg::trap {

void Print(const char* text) { g_syscall(G_PRINT, text); }

void Error(const char* text) {
    g_syscall(G_ERROR, text);
    std::abort();  // G_ERROR longjmps out of the module; never reached
}

int Argc() { return static_cast<int>(g_syscall(G_ARGC)); }

std::string Argv(int n) {
    char buf[MAX_STRING_CHARS];
    g_syscall(G_ARGV, n, buf, sizeof(buf));
    return buf;
}


void CvarRegister(vmCvar_t* cvar, const char* name, const char* value, int flags) {
    g_syscall(G_CVAR_REGISTER, cvar, name, value, flags);
}

void CvarUpdate(vmCvar_t* cvar) { g_syscall(G_CVAR_UPDATE, cvar); }
void CvarSet(const char* name, const char* value) { g_syscall(G_CVAR_SET, name, value); }
int CvarInt(const char* name) { return static_cast<int>(g_syscall(G_CVAR_VARIABLE_INTEGER_VALUE, name)); }

void LocateGameData(gentity_t* ents, int numEnts, int entSize, playerState_t* clients, int clientSize) {
    g_syscall(G_LOCATE_GAME_DATA, ents, numEnts, entSize, clients, clientSize);
}

void SendServerCommand(int clientNum, const char* text) { g_syscall(G_SEND_SERVER_COMMAND, clientNum, text); }
void SetConfigstring(int num, const char* value) { g_syscall(G_SET_CONFIGSTRING, num, value); }

std::string GetUserinfo(int clientNum) {
    char buf[MAX_INFO_STRING];
    g_syscall(G_GET_USERINFO, clientNum, buf, sizeof(buf));
    return buf;
}

void SetBrushModel(gentity_t* ent, const char* name) { g_syscall(G_SET_BRUSH_MODEL, ent, name); }

int EntitiesInBox(const vec3_t mins, const vec3_t maxs, int* list, int maxcount) {
    return static_cast<int>(g_syscall(G_ENTITIES_IN_BOX, mins, maxs, list, maxcount));
}

bool EntityContact(const vec3_t mins, const vec3_t maxs, const gentity_t* ent) {
    return g_syscall(G_ENTITY_CONTACT, mins, maxs, ent) != 0;
}

void LinkEntity(gentity_t* ent) { g_syscall(G_LINKENTITY, ent); }
void UnlinkEntity(gentity_t* ent) { g_syscall(G_UNLINKENTITY, ent); }

void Trace(trace_t* results, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end,
           int passEntityNum, int contentmask) {
    g_syscall(G_TRACE, results, start, mins, maxs, end, passEntityNum, contentmask);
}

int PointContents(const vec3_t point, int passEntityNum) {
    return static_cast<int>(g_syscall(G_POINT_CONTENTS, point, passEntityNum));
}

void GetUsercmd(int clientNum, usercmd_t* cmd) { g_syscall(G_GET_USERCMD, clientNum, cmd); }

bool GetEntityToken(char* buffer, int size) { return g_syscall(G_GET_ENTITY_TOKEN, buffer, size) != 0; }

bool FileRead(const char* path, std::string& out) {
    fileHandle_t f;
    const int len = static_cast<int>(g_syscall(G_FS_FOPEN_FILE, path, &f, FS_READ));
    if (len < 0 || !f) {
        return false;
    }
    out.resize(len);
    g_syscall(G_FS_READ, out.data(), len, f);
    g_syscall(G_FS_FCLOSE_FILE, f);
    return true;
}

bool FileWrite(const char* path, const std::string& data, bool append) {
    fileHandle_t f;
    g_syscall(G_FS_FOPEN_FILE, path, &f, append ? FS_APPEND : FS_WRITE);
    if (!f) {
        return false;
    }
    g_syscall(G_FS_WRITE, data.data(), static_cast<int>(data.size()), f);
    g_syscall(G_FS_FCLOSE_FILE, f);
    return true;
}

int RealTime(qtime_t* t) { return static_cast<int>(g_syscall(G_REAL_TIME, t)); }

std::vector<std::string> FileList(const char* dir, const char* extension) {
    static char buf[64 * 1024];
    const int count = static_cast<int>(g_syscall(G_FS_GETFILELIST, dir, extension, buf, sizeof(buf)));
    std::vector<std::string> out;
    const char* p = buf;
    for (int i = 0; i < count && p < buf + sizeof(buf); i++) {
        out.emplace_back(p);
        p += out.back().size() + 1;
    }
    return out;
}

}  // namespace sg::trap
