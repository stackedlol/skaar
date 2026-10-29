// Server game -> engine calls (gameImport_t in code/game/g_public.h).
//
// This file is part of Skaar. GPLv2 or later.
#pragma once

#include <string>
#include <vector>

#include "../shared/q3.h"

extern "C" {
#include "game/g_public.h"

// called by the reused bg_*.c code; defined in syscalls.cpp
void trap_Cvar_VariableStringBuffer(const char* name, char* buffer, int size);
void trap_SnapVector(float* v);
}

namespace sg {
struct gentity_t;
}

namespace sg::trap {

void Print(const char* text);
[[noreturn]] void Error(const char* text);
int Argc();
std::string Argv(int n);
void CvarRegister(vmCvar_t* cvar, const char* name, const char* value, int flags);
void CvarUpdate(vmCvar_t* cvar);
void CvarSet(const char* name, const char* value);
int CvarInt(const char* name);
void LocateGameData(gentity_t* ents, int numEnts, int entSize, playerState_t* clients, int clientSize);
void SendServerCommand(int clientNum, const char* text);
void SetConfigstring(int num, const char* value);
std::string GetUserinfo(int clientNum);
void SetBrushModel(gentity_t* ent, const char* name);
int EntitiesInBox(const vec3_t mins, const vec3_t maxs, int* list, int maxcount);
bool EntityContact(const vec3_t mins, const vec3_t maxs, const gentity_t* ent);
void LinkEntity(gentity_t* ent);
void UnlinkEntity(gentity_t* ent);
void Trace(trace_t* results, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end,
           int passEntityNum, int contentmask);
int PointContents(const vec3_t point, int passEntityNum);
void GetUsercmd(int clientNum, usercmd_t* cmd);
bool GetEntityToken(char* buffer, int size);
bool FileRead(const char* path, std::string& out);
bool FileWrite(const char* path, const std::string& data, bool append);
int RealTime(qtime_t* t);
std::vector<std::string> FileList(const char* dir, const char* extension);

}  // namespace sg::trap
