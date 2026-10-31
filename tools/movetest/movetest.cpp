// Headless movement tests (P5, docs/cpm-physics.md). No engine: a small
// box-vs-convex-brush trace stands in for the collision model.
//
//   tools/movetest/run.sh        builds and runs; exit status 0 = all passed
//
// 1. Parity: the C++ port with the vq3 ruleset gives byte-identical player
//    states to Q3's C bg_pmove.c on scripted runs.
// 2. Physics: ground speed cap, strafe-jump gain, cpm strafe-only air accel,
//    air control, ramp and double jumps.
// 3. Determinism and ruleset JSON parsing.
//
// This file is part of Skaar. GPLv2 or later.
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "shared/move_pmove.h"
#include "shared/move_ruleset.h"

extern "C" {
void Pmove(pmove_t* pmove);  // the C original, code/game/bg_pmove.c

// what the modules normally provide to the reused Q3 C code
void QDECL Com_Error(int, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::abort();
}
void QDECL Com_Printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
}
void QDECL Com_DPrintf(const char*, ...) {}
// the engine's snap on arm64 (q_shared.h Q_SnapVector); the port uses std::round too
void trap_SnapVector(float* v) {
    v[0] = std::round(v[0]);
    v[1] = std::round(v[1]);
    v[2] = std::round(v[2]);
}
void trap_Cvar_VariableStringBuffer(const char*, char* buffer, int size) {
    if (size > 0) {
        buffer[0] = 0;
    }
}
}

namespace {

using namespace skaar::move;

// ---- world ------------------------------------------------------------------

constexpr float kClipEpsilon = 0.125f;  // SURFACE_CLIP_EPSILON

struct Plane {
    vec3_t normal;
    float dist;  // inside: dot(p, normal) < dist
};

struct Brush {
    std::vector<Plane> planes;  // the 6 axial sides first, then any others
};

std::vector<Brush> g_world;

Brush Box(float x0, float y0, float z0, float x1, float y1, float z1) {
    Brush b;
    b.planes = {{{1, 0, 0}, x1}, {{-1, 0, 0}, -x0}, {{0, 1, 0}, y1},
                {{0, -1, 0}, -y0}, {{0, 0, 1}, z1}, {{0, 0, -1}, -z0}};
    return b;
}

// A ramp rising along +x from (x0, z=0) to (x1, z=height).
Brush Ramp(float x0, float x1, float y0, float y1, float height) {
    Brush b = Box(x0, y0, -16, x1, y1, height);
    vec3_t n = {-height, 0, x1 - x0};
    VectorNormalize(n);
    b.planes.push_back({{n[0], n[1], n[2]}, n[0] * x0});
    return b;
}

void Floor() {
    g_world.push_back(Box(-100000, -100000, -16, 100000, 100000, 0));
}

// Box sweep against convex brushes, the same idea as CM_TraceThroughBrush.
void Trace(trace_t* tr, const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int, int) {
    std::memset(tr, 0, sizeof(*tr));
    tr->fraction = 1;
    tr->entityNum = ENTITYNUM_NONE;

    for (const Brush& brush : g_world) {
        float enter = -1, leave = 1;
        bool startOut = false, getOut = false;
        const Plane* clip = nullptr;
        bool miss = false;

        for (const Plane& p : brush.planes) {
            // push the plane out by the box's extent along the normal
            float dist = p.dist;
            for (int j = 0; j < 3; j++) {
                dist -= p.normal[j] * (p.normal[j] < 0 ? maxs[j] : mins[j]);
            }
            const float d1 = DotProduct(start, p.normal) - dist;
            const float d2 = DotProduct(end, p.normal) - dist;
            if (d2 > 0) {
                getOut = true;
            }
            if (d1 > 0) {
                startOut = true;
            }
            if (d1 > 0 && (d2 >= kClipEpsilon || d2 >= d1)) {
                miss = true;  // completely in front of this face
                break;
            }
            if (d1 <= 0 && d2 <= 0) {
                continue;
            }
            if (d1 > d2) {
                const float f = std::max((d1 - kClipEpsilon) / (d1 - d2), 0.0f);
                if (f > enter) {
                    enter = f;
                    clip = &p;
                }
            } else {
                const float f = std::min((d1 + kClipEpsilon) / (d1 - d2), 1.0f);
                leave = std::min(leave, f);
            }
        }
        if (miss) {
            continue;
        }
        if (!startOut) {
            tr->startsolid = qtrue;
            if (!getOut) {
                tr->allsolid = qtrue;
                tr->fraction = 0;
            }
            continue;
        }
        if (enter < leave && enter > -1 && enter < tr->fraction && clip) {
            tr->fraction = std::max(enter, 0.0f);
            VectorCopy(clip->normal, tr->plane.normal);
            tr->plane.dist = clip->dist;
            tr->entityNum = ENTITYNUM_WORLD;
        }
    }

    for (int i = 0; i < 3; i++) {
        tr->endpos[i] = start[i] + tr->fraction * (end[i] - start[i]);
    }
}

// A player standing exactly on the floor plane (origin z == 24) is allsolid to
// the stand-in Trace above, because d1 == 0 does not count as "outside"; ground
// movement only works once the gravity step has lifted him by the clip epsilon.
// WaterMove applies no gravity, so players that start in water start settled.
constexpr float kSettledZ = 24.125f;  // 24 + SURFACE_CLIP_EPSILON

// What PointContents reports. Off by default; a script turns water on so the
// water paths in the port are reachable at all. The pool fills everything below
// g_waterZ and left of g_waterX; past g_wallX (below g_wallZ) is a low ledge,
// which is what CheckWaterJump probes for before it launches the player.
float g_waterZ = -100000;  // water surface height
float g_waterX = 100000;   // the pool ends here
float g_wallX = 100000;    // the ledge starts here
float g_wallZ = 0;         // ... up to this height

int PointContents(const vec3_t p, int) {
    if (p[0] > g_wallX && p[2] < g_wallZ) {
        return CONTENTS_SOLID;
    }
    return p[2] < g_waterZ && p[0] < g_waterX ? CONTENTS_WATER : 0;
}

void ResetStubs() {
    g_waterZ = -100000;
    g_waterX = 100000;
    g_wallX = 100000;
    g_wallZ = 0;
}

// ---- player -----------------------------------------------------------------

constexpr int kMsec = 8;  // pmove_fixed 1, pmove_msec 8 (plan-P1)
// gravity one 8 ms frame of SlideMove applies at gravity 800; reading a jump
// velocity back has to add exactly this much
constexpr float kGravFrame = 800.0f * kMsec * 0.001f;

struct Player {
    playerState_t ps{};
    int time = 0;
    bool useC = false;  // run Q3's C Pmove instead of the port
    // one command covers `period` ms but is still chopped up into pmove_msec
    // steps, which is what the shipped modules do at any fps below 125
    int period = kMsec;
    bool fixed = true;           // pmove_fixed
    int waterlevel = 0;          // pm->waterlevel after the last step
    int watertype = 0;           // pm->watertype after the last step

    Player(int ruleset, float x, float y, float z) {
        ps.commandTime = 0;
        ps.pm_type = PM_NORMAL;
        ps.gravity = 800;
        ps.speed = 320;
        ps.stats[STAT_HEALTH] = 100;
        ps.stats[STAT_MAX_HEALTH] = 100;
        ps.stats[kStatRuleset] = ruleset;
        ps.groundEntityNum = ENTITYNUM_NONE;
        ps.clientNum = 0;
        VectorSet(ps.origin, x, y, z);
    }

    // one command; yaw in degrees
    void Step(float yaw, int forward, int right, int up, float pitch = 0) {
        time += period;
        pmove_t pm{};
        pm.ps = &ps;
        pm.cmd.serverTime = time;
        pm.cmd.angles[YAW] = ANGLE2SHORT(yaw);
        pm.cmd.angles[PITCH] = ANGLE2SHORT(pitch);
        pm.cmd.forwardmove = static_cast<signed char>(forward);
        pm.cmd.rightmove = static_cast<signed char>(right);
        pm.cmd.upmove = static_cast<signed char>(up);
        pm.tracemask = MASK_PLAYERSOLID;
        pm.trace = Trace;
        pm.pointcontents = PointContents;
        pm.noFootsteps = qtrue;
        pm.pmove_fixed = fixed ? 1 : 0;
        pm.pmove_msec = kMsec;
        if (useC) {
            ::Pmove(&pm);
        } else {
            skaar::move::Pmove(&pm);
        }
        waterlevel = pm.waterlevel;
        watertype = pm.watertype;
    }

    float Speed() const { return std::sqrt(ps.velocity[0] * ps.velocity[0] + ps.velocity[1] * ps.velocity[1]); }
    float VelYaw() const { return RAD2DEG(std::atan2(ps.velocity[1], ps.velocity[0])); }
    bool OnGround() const { return ps.groundEntityNum != ENTITYNUM_NONE; }
};

// ---- checks -----------------------------------------------------------------

int g_failed = 0;
int g_passed = 0;

void Check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "pass" : "FAIL", what, detail.empty() ? "" : "  ", detail.c_str());
    (ok ? g_passed : g_failed)++;
}

std::string Fmt(const char* fmt, ...) Q_PRINTF_FUNC(1, 2);
std::string Fmt(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// Strafe-jump bot: forward + right, view turned so the wish direction sits at
// the angle that adds the most speed, jump pressed just before each landing.
void StrafeJump(Player& p, float seconds) {
    const int steps = static_cast<int>(seconds * 1000 / kMsec);
    for (int i = 0; i < steps; i++) {
        const float v = std::max(p.Speed(), 1.0f);
        const float accelspeed = (p.OnGround() ? 0 : 1) * 1.0f * kMsec * 0.001f * 320;
        const float cosTheta = std::clamp((320 - accelspeed) / v, -1.0f, 1.0f);
        const float theta = RAD2DEG(std::acos(cosTheta));
        const float yaw = p.VelYaw() - theta + 45;  // forward+right wishes 45° right of the view
        const int jump = (p.OnGround() || p.ps.velocity[2] < 0) ? 127 : 0;  // release while rising
        p.Step(yaw, 127, 127, jump);
    }
}

// Run straight to full ground speed.
void RunUp(Player& p, float yaw, float seconds) {
    for (int i = 0; i < static_cast<int>(seconds * 1000 / kMsec); i++) {
        p.Step(yaw, 127, 0, 0);
    }
}

// ---- tests ------------------------------------------------------------------

// Same scripted input through both implementations, comparing every step.
void TestParity() {
    struct Script {
        const char* name;
        std::function<void()> world;
        std::function<void(Player&, int)> input;
        int steps;
        int period = kMsec;  // command length; > pmove_msec chops it into sub-steps
        bool fixed = true;   // pmove_fixed
    };
    const std::vector<Script> scripts = {
        {"parity: run, stop, crouch-walk", Floor,
         [](Player& p, int i) { p.Step(10.0f + static_cast<float>(i) * 0.1f, i < 150 ? 127 : 0, i % 90 < 30 ? -127 : 0, i > 200 && i < 260 ? -127 : 0); },
         320},
        {"parity: strafe jumping", Floor,
         [](Player& p, int i) {
             if (i < 120) {
                 p.Step(0, 127, 0, 0);
                 return;
             }
             const float v = std::max(p.Speed(), 1.0f);
             const float theta = RAD2DEG(std::acos(std::clamp((320 - 2.56f) / v, -1.0f, 1.0f)));
             p.Step(p.VelYaw() - theta + 45, 127, 127, (p.OnGround() || p.ps.velocity[2] < 0) ? 127 : 0);
         },
         900},
        {"parity: up and over a ramp, walls", [] {
             Floor();
             g_world.push_back(Ramp(200, 500, -200, 200, 110));
             g_world.push_back(Box(500, -200, -16, 900, 200, 110));
             g_world.push_back(Box(-300, 250, -16, 1200, 300, 300));
         },
         [](Player& p, int i) { p.Step(8.0f, 127, i % 200 < 100 ? 50 : -50, (i / 40) % 4 == 0 ? 127 : 0); }, 600},
        {"parity: steps", [] {
             Floor();
             for (int s = 0; s < 6; s++) {
                 g_world.push_back(Box(100 + 32.0f * s, -100, -16, 1000, 100, 16.0f * (s + 1)));
             }
         },
         [](Player& p, int) { p.Step(0, 127, 0, 0); }, 300},
        // a whole render frame per command at 30-90 fps: Pmove's chop loop runs
        // 2-5 sub-steps and has to re-issue a held jump for each of them
        {"parity: 33 ms commands, strafe jumping", Floor,
         [](Player& p, int i) {
             if (i < 40) {
                 p.Step(0, 127, 0, 0);
                 return;
             }
             const float v = std::max(p.Speed(), 1.0f);
             const float theta = RAD2DEG(std::acos(std::clamp((320 - 2.56f) / v, -1.0f, 1.0f)));
             p.Step(p.VelYaw() - theta + 45, 127, 127, (p.OnGround() || p.ps.velocity[2] < 0) ? 127 : 0);
         },
         300, 33},
        {"parity: 100 ms commands, pmove_fixed 0", Floor,
         [](Player& p, int) {
             const float v = std::max(p.Speed(), 1.0f);
             const float theta = RAD2DEG(std::acos(std::clamp((320 - 2.56f) / v, -1.0f, 1.0f)));
             p.Step(p.VelYaw() - theta + 45, 127, 127, (p.OnGround() || p.ps.velocity[2] < 0) ? 127 : 0);
         },
         200, 100, false},
        // the state branches the constructor used to make unreachable: every
        // pm_type, PW_FLIGHT's friction and the ±16000 pitch clamp
        {"parity: spectator", Floor,
         [](Player& p, int i) { p.ps.pm_type = PM_SPECTATOR; p.Step(static_cast<float>(i) * 3.0f, 127, 0, 0); },
         200},
        {"parity: noclip", Floor,
         [](Player& p, int i) { p.ps.pm_type = PM_NOCLIP; p.Step(static_cast<float>(i) * 3.0f, 127, 0, 0); },
         200},
        {"parity: freeze", Floor,
         [](Player& p, int i) { p.ps.pm_type = PM_FREEZE; p.Step(static_cast<float>(i) * 3.0f, 127, 127, 0); },
         200},
        {"parity: dead", Floor,
         [](Player& p, int i) {
             p.ps.pm_type = PM_DEAD;
             p.ps.stats[STAT_HEALTH] = 0;
             if (i == 0) {
                 // a corpse that is still moving, or DeadMove is a no-op
                 p.ps.velocity[0] = 300;
                 p.ps.velocity[2] = 120;
             }
             p.Step(static_cast<float>(i) * 3.0f, 0, 0, 0);
         },
         200},
        {"parity: flight powerup", Floor,
         [](Player& p, int i) { p.ps.powerups[PW_FLIGHT] = 1; p.Step(static_cast<float>(i) * 3.0f, 127, 0, i % 20 < 10 ? 127 : 0); },
         200},
        {"parity: pitch past the clamp", Floor,
         [](Player& p, int i) { p.Step(static_cast<float>(i) * 3.0f, 127, 0, 0, 89.0f); }, 200},
        {"parity: wade through a pool", [] {
             Floor();
             g_waterZ = 40;   // ankle to chest deep for a standing player
             g_waterX = 300;  // ... and dry land past x = 300
         },
         [](Player& p, int i) {
             if (i == 0) {
                 p.ps.origin[2] = kSettledZ;
             }
             p.Step(0, 127, 0, (i % 40) < 8 ? 127 : 0);
         },
         400},
    };

    for (const Script& s : scripts) {
        g_world.clear();
        ResetStubs();
        s.world();
        Player cpp(0, 0, 0, 24);
        Player c(0, 0, 0, 24);
        c.useC = true;
        cpp.period = c.period = s.period;
        cpp.fixed = c.fixed = s.fixed;
        int firstDiff = -1;
        int maxWater = 0;
        for (int i = 0; i < s.steps && firstDiff < 0; i++) {
            s.input(cpp, i);
            s.input(c, i);
            maxWater = std::max(maxWater, cpp.waterlevel);
            // waterlevel/watertype are pmove_t fields, so the playerState memcmp
            // below cannot see them
            if (cpp.waterlevel != c.waterlevel || cpp.watertype != c.watertype ||
                std::memcmp(&cpp.ps, &c.ps, sizeof(playerState_t)) != 0) {
                firstDiff = i;
            }
        }
        Check(firstDiff < 0, s.name,
              firstDiff < 0 ? Fmt("%d steps identical, end speed %.0f", s.steps, cpp.Speed())
                            : Fmt("differs at step %d: c++ (%.3f %.3f %.3f) v %.3f, c (%.3f %.3f %.3f) v %.3f",
                                  firstDiff, cpp.ps.origin[0], cpp.ps.origin[1], cpp.ps.origin[2], cpp.Speed(),
                                  c.ps.origin[0], c.ps.origin[1], c.ps.origin[2], c.Speed()));
        if (s.name == std::string("parity: wade through a pool")) {
            Check(maxWater == 2 && cpp.ps.origin[0] > 300,
                  "parity: the pool gives waterlevel 2 and the player swims out of it",
                  Fmt("max waterlevel %d, end x %.0f", maxWater, cpp.ps.origin[0]));
        }
    }
    g_world.clear();
    ResetStubs();  // later tests assume dry land
}

void TestGroundSpeed() {
    for (int rs : {0, 1}) {
        g_world.clear();
        Floor();
        Player p(rs, 0, 0, 24);
        RunUp(p, 0, 2);
        Check(std::fabs(p.Speed() - 320) < 1.5f, Fmt("%s: ground speed caps at 320", RulesetAt(rs).id.c_str()).c_str(),
              Fmt("%.1f", p.Speed()));
    }
}

void TestStrafeJump() {
    float end[2];
    for (int rs : {0, 1}) {
        g_world.clear();
        Floor();
        Player p(rs, 0, 0, 24);
        RunUp(p, 0, 1);
        StrafeJump(p, 8);
        end[rs] = p.Speed();
    }
    Check(end[0] > 450, "vq3: strafe jumping gains speed past 320", Fmt("%.0f ups after 8 s", end[0]));
    Check(end[1] > 450, "cpm: strafe jumping gains speed past 320", Fmt("%.0f ups after 8 s", end[1]));
}

// Free fall, only strafe keys, wishing at right angles to the velocity: the
// velocity turns without losing speed. cpm's strafe-only accel (70, cap 30)
// turns far faster than vq3's (1, 320). Raw speed gain isn't the point: per
// frame vq3 adds more to v^2 (2*2.56*317.4 vs 2*16.8*13.2).
void TestAirStrafe() {
    float turned[2], speed[2];
    for (int rs : {0, 1}) {
        g_world.clear();
        Player p(rs, 0, 0, 100000);
        p.ps.velocity[0] = 500;
        float total = 0;
        for (int i = 0; i < 62; i++) {
            const float before = p.VelYaw();
            p.Step(before, 0, 127, 0);  // right-only wishes 90° right of the view
            total += before - p.VelYaw();
        }
        turned[rs] = total;
        speed[rs] = p.Speed();
    }
    Check(turned[1] > 4 * turned[0] && speed[1] >= 499, "cpm: strafe-only air turns faster, keeping speed",
          Fmt("0.5 s at 500 ups: vq3 %.1f°, cpm %.1f° (cpm speed %.0f)", turned[0], turned[1], speed[1]));
}

// Forward only, looking 30° left of the velocity, in free fall.
void TestAirControl() {
    float yaw[2], speed[2];
    for (int rs : {0, 1}) {
        g_world.clear();
        Player p(rs, 0, 0, 100000);
        p.ps.velocity[0] = 400;
        for (int i = 0; i < 62; i++) {
            p.Step(30, 127, 0, 0);
        }
        yaw[rs] = p.VelYaw();
        speed[rs] = p.Speed();
    }
    Check(std::fabs(yaw[0]) < 1, "vq3: no air control", Fmt("velocity turned %.1f°", yaw[0]));
    Check(yaw[1] > 10 && yaw[1] <= 30.5f, "cpm: air control turns toward the view",
          Fmt("velocity turned %.1f° in 0.5 s", yaw[1]));
    Check(std::fabs(speed[1] - 400) < 4, "cpm: air control keeps horizontal speed", Fmt("%.1f ups", speed[1]));
}

// Water: the levels SetWaterLevel computes, wading out of a pool, and the water
// jump CheckWaterJump launches. Every one of these was dead code while
// PointContents always returned 0.
void TestWater() {
    g_world.clear();
    ResetStubs();
    Floor();
    g_waterZ = 40;  // chest deep for a standing player (box z 0..56)
    Player shallow(0, 0, 0, kSettledZ);
    shallow.Step(0, 0, 0, 0);
    Check(shallow.waterlevel == 2 && shallow.watertype == CONTENTS_WATER,
          "water: standing chest deep is waterlevel 2",
          Fmt("level %d, type %d", shallow.waterlevel, shallow.watertype));

    g_waterZ = 100;  // over the player's head
    Player deep(0, 0, 0, kSettledZ);
    deep.Step(0, 0, 0, 0);
    Check(deep.waterlevel == 3, "water: fully submerged is waterlevel 3", Fmt("level %d", deep.waterlevel));

    // wade out: the pool ends at x = 300
    ResetStubs();
    g_waterZ = 40;
    g_waterX = 300;
    Player out(0, 0, 0, kSettledZ);
    int inWater = -1;
    for (int i = 0; i < 400; i++) {
        out.Step(0, 127, 0, 0);
        if (inWater < 0 && out.ps.origin[0] > 200) {
            inWater = out.waterlevel;
        }
    }
    Check(inWater == 2 && out.ps.origin[0] > 320 && out.waterlevel == 0,
          "water: walking out of the pool clears the waterlevel",
          Fmt("level %d at x 200, level %d at x %.0f", inWater, out.waterlevel, out.ps.origin[0]));

    // water jump: waterlevel 2 with a low ledge 30 units ahead of the view
    ResetStubs();
    g_waterZ = 40;
    g_wallX = 20;
    g_wallZ = 32;
    Player jump(0, 0, 0, kSettledZ);
    for (int i = 0; i < 5 && !(jump.ps.pm_flags & PMF_TIME_WATERJUMP); i++) {
        jump.Step(0, 127, 0, 0);
    }
    // CheckWaterJump sets 350 ups; StepSlideMove and WaterJumpMove then each take
    // a frame of gravity off it before the snap
    Check((jump.ps.pm_flags & PMF_TIME_WATERJUMP) != 0 &&
              std::fabs(jump.ps.velocity[2] - (350 - 2 * kGravFrame)) <= 1 && std::fabs(jump.Speed() - 200) < 1,
          "water: a ledge in front turns into a water jump",
          Fmt("vz %.0f (350 less two frames), forward %.0f ups, pm_flags %d", jump.ps.velocity[2], jump.Speed(),
              jump.ps.pm_flags));
    ResetStubs();
}

// Jump velocity straight off the ground.
float JumpVz(int rs, int jumpTimeMs, float vz0 = 0) {
    g_world.clear();
    ResetStubs();
    Floor();
    Player p(rs, 0, 0, 24);
    p.Step(0, 0, 0, 0);  // settle on the ground
    p.ps.stats[kStatJumpTime] = jumpTimeMs;
    p.ps.velocity[2] = vz0;
    p.Step(0, 0, 0, 127);
    // SlideMove copies endVelocity (vz - gravity * a whole frametime) back into
    // ps.velocity before the snap, so undo one full frame, not half of one
    return p.ps.velocity[2] + kGravFrame;
}

void TestJumps() {
    Check(std::fabs(JumpVz(0, 0) - 270) < 1, "vq3: jump velocity 270", Fmt("%.1f", JumpVz(0, 0)));
    Check(std::fabs(JumpVz(1, 0) - 270) < 1, "cpm: first jump 270", Fmt("%.1f", JumpVz(1, 0)));
    Check(std::fabs(JumpVz(1, 300) - 370) < 1, "cpm: jump inside the 400 ms window +100", Fmt("%.1f", JumpVz(1, 300)));
    Check(std::fabs(JumpVz(0, 300) - 270) < 1, "vq3: no double jump", Fmt("%.1f", JumpVz(0, 300)));

    // low ceiling: bonk, land fast, jump again inside the window
    for (int rs : {0, 1}) {
        g_world.clear();
        Floor();
        g_world.push_back(Box(-500, -500, 76, 500, 500, 100));  // 20 units above the head
        Player p(rs, 0, 0, 24);
        p.Step(0, 0, 0, 0);
        p.Step(0, 0, 0, 127);  // first jump
        int landedAt = -1;
        float second = 0;
        for (int i = 0; i < 200; i++) {
            const bool ground = p.OnGround();
            if (ground && landedAt < 0) {
                landedAt = p.time;
            }
            p.Step(0, 0, 0, ground ? 127 : 0);
            if (ground) {
                second = p.ps.velocity[2] + kGravFrame;
                break;
            }
        }
        const float want = rs == 1 ? 370 : 270;
        Check(std::fabs(second - want) < 1,
              Fmt("%s: ceiling bonk, second jump %s", RulesetAt(rs).id.c_str(), rs ? "is a double jump" : "is normal")
                  .c_str(),
              Fmt("landed after %d ms, jumped at %.1f", landedAt, second));
    }

    // ramp: run up a ~20° ramp and jump halfway
    float rampVz[2];
    for (int rs : {0, 1}) {
        g_world.clear();
        Floor();
        g_world.push_back(Ramp(300, 1100, -300, 300, 291));  // tan(20°) * 800
        Player p(rs, -200, 0, 24);
        RunUp(p, 0, 1.8f);
        float vzBefore = p.ps.velocity[2];
        p.Step(0, 127, 0, 127);
        rampVz[rs] = p.ps.velocity[2] + kGravFrame;
        if (rs == 1) {
            Check(vzBefore > 80 && std::fabs(rampVz[1] - (270 + vzBefore)) < 6, "cpm: ramp jump keeps upward speed",
                  Fmt("vz %.0f before, %.0f after", vzBefore, rampVz[1]));
        }
    }
    Check(std::fabs(rampVz[0] - 270) < 6, "vq3: ramp jump resets to 270", Fmt("%.0f", rampVz[0]));

    // downhill: run down the ramp, the jump isn't weakened
    g_world.clear();
    Floor();
    g_world.push_back(Ramp(300, 1100, -300, 300, 291));
    Player down(1, 1000, 0, 291 * 700 / 800.0f + 30);
    RunUp(down, 180, 1.0f);
    const float vzBefore = down.ps.velocity[2];
    down.Step(180, 127, 0, 127);
    const float vz = down.ps.velocity[2] + kGravFrame;
    Check(vzBefore < -50 && std::fabs(vz - 270) < 6, "cpm: downhill jump is a full 270",
          Fmt("vz %.0f before, %.0f after", vzBefore, vz));
}

void TestDeterminism() {
    playerState_t a{}, b{};
    for (playerState_t* out : {&a, &b}) {
        g_world.clear();
        Floor();
        g_world.push_back(Ramp(600, 1000, -300, 300, 140));
        Player p(1, 0, 0, 24);
        RunUp(p, 0, 0.5f);
        StrafeJump(p, 4);
        *out = p.ps;
    }
    Check(std::memcmp(&a, &b, sizeof(a)) == 0, "cpm: same inputs, same playerState");
}

void TestRulesetJson() {
    Ruleset r;
    std::string error;
    bool ok = ParseRuleset("cpm", R"({ // comment
        "label": "Air", "airControl": 120, "doubleJumpWindowMs": 300 })",
                           r, error);
    Check(ok && r.airControl == 120 && r.doubleJumpWindowMs == 300 && r.strafeAccelerate == 70 && r.label == "Air",
          "json: overrides keep the other built-in values", error);

    ok = ParseRuleset("slidey", R"({ "friction": 2 })", r, error);
    Check(ok && r.friction == 2 && r.airControl == 0 && r.label == "slidey", "json: a new id starts from vq3", error);

    ok = ParseRuleset("bad", R"({ "friction": -1 })", r, error);
    Check(!ok && !error.empty(), "json: out-of-range value is rejected", error);

    ok = ParseRuleset("bad", R"([1, 2])", r, error);
    Check(!ok, "json: non-object is rejected", error);

    ResetRulesets();
    Ruleset custom = Vq3();
    custom.id = "slidey";
    const int index = AddRuleset(custom);
    Check(index == 2 && FindRuleset("slidey") == 2 && RulesetAt(99).id == "vq3", "registry: add, find, out-of-range");

    // the server's configstring rebuilds the same ruleset on the client
    Ruleset odd = Cpm();
    odd.id = "odd";
    odd.label = "Odd one";
    odd.airControl = 123.456f;
    odd.friction = 0.1f;
    odd.autoHop = true;
    const int oddIndex = AddRuleset(odd);
    const std::string cs = EncodeRuleset(oddIndex);
    ResetRulesets();
    ok = DecodeRuleset(cs, error);
    const Ruleset& back = RulesetAt(oddIndex);
    Check(ok && back.id == "odd" && back.label == odd.label && back.airControl == odd.airControl &&
              back.friction == odd.friction && back.autoHop && ToJson(back) == ToJson(odd),
          "sync: configstring round-trips exactly", Fmt("%zu chars", cs.size()));
    Check(cs.find('"') == std::string::npos, "sync: configstring has no quotes (survives the cs command)");
    Check(!DecodeRuleset("\\index\\x\\id\\a", error) && !DecodeRuleset("\\index\\3", error) &&
              !DecodeRuleset("\\index\\3\\id\\a\\friction\\-5", error),
          "sync: malformed configstring is rejected", error);
    odd.label = "say \"hi\"; ok\\";
    AddRuleset(odd);
    Check(DecodeRuleset(EncodeRuleset(oddIndex), error) && RulesetAt(oddIndex).label == "say  hi   ok ",
          "sync: label characters info strings can't carry become spaces", RulesetAt(oddIndex).label);

    // loading files: name order, bad files reported
    std::vector<std::string> errors;
    LoadRulesets([](const char*, const char*) { return std::vector<std::string>{"zz.json", "cpm.json", "broken.json"}; },
                 [](const std::string& path, std::string& text) {
                     text = path == "rulesets/broken.json" ? "{" : path == "rulesets/cpm.json" ? R"({"airControl": 99})" : "{}";
                     return true;
                 },
                 errors);
    Check(errors.size() == 1 && RulesetAt(1).airControl == 99 && FindRuleset("zz") == 2,
          "load: overrides by id, appends new, reports bad files", errors.empty() ? "" : errors[0]);
    ResetRulesets();
}

// SetWeaponRefire overrides Q3's refire table; clearing it restores Q3's.
void TestWeaponRefire() {
    auto fire = [] {
        g_world.clear();
        Floor();
        Player p(0, 0, 0, 24);
        p.ps.weapon = WP_RAILGUN;
        p.ps.stats[STAT_WEAPONS] = 1 << WP_RAILGUN;
        p.ps.ammo[WP_RAILGUN] = -1;
        p.ps.weaponstate = WEAPON_READY;
        p.time += kMsec;
        pmove_t pm{};
        pm.ps = &p.ps;
        pm.cmd.serverTime = p.time;
        pm.cmd.buttons = BUTTON_ATTACK;
        pm.cmd.weapon = WP_RAILGUN;
        pm.tracemask = MASK_PLAYERSOLID;
        pm.trace = Trace;
        pm.pointcontents = PointContents;
        pm.pmove_fixed = 1;
        pm.pmove_msec = kMsec;
        skaar::move::Pmove(&pm);
        return p.ps.weaponTime;
    };
    const int q3 = fire();
    SetWeaponRefire(WP_RAILGUN, 300);
    const int custom = fire();
    ClearWeaponRefire();
    const int back = fire();
    Check(q3 == 1500 && custom == 300 && back == 1500, "weapons: refire override, then Q3's table again",
          Fmt("q3 %d, override %d, cleared %d", q3, custom, back));
}

// data/base/rulesets/*.json must match the compiled-in defaults.
void TestShippedRulesets(const std::string& repo) {
    for (const Ruleset* builtin : {&Vq3(), &Cpm()}) {
        const std::string path = repo + "/data/base/rulesets/" + builtin->id + ".json";
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        Ruleset parsed;
        std::string error;
        const bool ok = in.good() && ParseRuleset(builtin->id, text.str(), parsed, error);
        Check(ok && ToJson(parsed) == ToJson(*builtin), Fmt("data: rulesets/%s.json matches the built-in", builtin->id.c_str()).c_str(),
              ok ? "" : (error.empty() ? "can't read " + path : error));
    }
}

// rulesets/tac.json: run speed, counter-strafe stops, no strafe-jump gain.
void TestTac(const std::string& repo) {
    std::ifstream in(repo + "/data/base/rulesets/tac.json");
    std::stringstream text;
    text << in.rdbuf();
    Ruleset tac;
    std::string error;
    if (!ParseRuleset("tac", text.str(), tac, error)) {
        Check(false, "tac: rulesets/tac.json parses", error);
        return;
    }
    ResetRulesets();
    const int rs = AddRuleset(tac);

    // run right to full speed, then counter-strafe (or let go) and time the stop
    auto stop = [rs](int counter, int* toThird, int* toZero) {
        g_world.clear();
        Floor();
        Player p(rs, 0, 0, 24);
        for (int i = 0; i < 150; i++) {
            p.Step(0, 0, 127, 0);
        }
        const float top = p.Speed();
        *toThird = *toZero = -1;
        for (int i = 1; i <= 100 && *toZero < 0; i++) {
            p.Step(0, 0, counter, 0);
            const float along = -p.ps.velocity[1];  // right is -y at yaw 0
            if (*toThird < 0 && along < top / 3) {
                *toThird = i * kMsec;
            }
            if (along <= 1) {
                *toZero = i * kMsec;
            }
        }
        return top;
    };
    int csThird, csZero, relThird, relZero;
    const float top = stop(-127, &csThird, &csZero);
    stop(0, &relThird, &relZero);
    Check(std::fabs(top - 260) < 1.5f, "tac: run speed 260", Fmt("%.1f", top));
    Check(csThird > 0 && csThird <= 64 && csZero > 0 && csZero <= 112, "tac: counter-strafe settles fast",
          Fmt("to 1/3 speed %d ms, stopped %d ms", csThird, csZero));
    Check(relZero > csZero, "tac: letting go stops slower than counter-strafing",
          Fmt("release: to 1/3 speed %d ms, stopped %d ms", relThird, relZero));

    g_world.clear();
    Floor();
    Player p(rs, 0, 0, 24);
    RunUp(p, 0, 1);
    StrafeJump(p, 8);
    Check(p.Speed() <= 262, "tac: no strafe-jump speed gain", Fmt("%.0f ups after 8 s", p.Speed()));

    const float vz = JumpVz(rs, 0);
    Check(std::fabs(vz - 260) < 1, "tac: jump velocity 260", Fmt("%.1f (~%.0f units high)", vz, 260 * 260 / 1600.0));
    ResetRulesets();
}

// Semi-automatic: one shot per press, refire waits for a release.
void TestSemiAuto() {
    g_world.clear();
    Floor();
    Player p(0, 0, 0, 24);
    p.ps.weapon = WP_MACHINEGUN;
    p.ps.stats[STAT_WEAPONS] = 1 << WP_MACHINEGUN;
    p.ps.ammo[WP_MACHINEGUN] = -1;
    p.ps.weaponstate = WEAPON_READY;
    int shots = 0;
    auto step = [&p, &shots](bool attack) {
        const int before = p.ps.eventSequence;
        p.time += kMsec;
        pmove_t pm{};
        pm.ps = &p.ps;
        pm.cmd.serverTime = p.time;
        pm.cmd.buttons = attack ? BUTTON_ATTACK : 0;
        pm.cmd.weapon = WP_MACHINEGUN;
        pm.tracemask = MASK_PLAYERSOLID;
        pm.trace = Trace;
        pm.pointcontents = PointContents;
        pm.pmove_fixed = 1;
        pm.pmove_msec = kMsec;
        skaar::move::Pmove(&pm);
        for (int e = before; e < p.ps.eventSequence; e++) {
            shots += (p.ps.events[e & (MAX_PS_EVENTS - 1)] & ~EV_EVENT_BITS) == EV_FIRE_WEAPON;
        }
    };
    auto hold = [&step](int ms) {
        for (int t = 0; t < ms; t += kMsec) {
            step(true);
        }
    };
    auto release = [&step](int ms) {
        for (int t = 0; t < ms; t += kMsec) {
            step(false);
        }
    };

    hold(500);
    const int automatic = shots;
    SetWeaponSemiAuto(WP_MACHINEGUN, true);
    release(200);
    shots = 0;
    hold(500);
    const int heldSemi = shots;
    shots = 0;
    for (int i = 0; i < 5; i++) {
        release(104);
        hold(104);
    }
    const int tapped = shots;
    ClearWeaponSemiAuto();
    Check(automatic >= 5 && heldSemi == 1 && tapped == 5, "weapons: semi-auto fires once per press",
          Fmt("held 0.5 s: auto %d shots, semi %d; 5 taps: %d", automatic, heldSemi, tapped));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        // without the repo root the shipped-ruleset and tac checks used to just
        // vanish, and the run still printed a green summary and exited 0
        std::printf("usage: movetest <repo root>\n");
        return 2;
    }
    TestParity();
    TestGroundSpeed();
    TestStrafeJump();
    TestAirStrafe();
    TestAirControl();
    TestWater();
    TestJumps();
    TestDeterminism();
    TestRulesetJson();
    TestWeaponRefire();
    TestSemiAuto();
    TestShippedRulesets(argv[1]);
    TestTac(argv[1]);
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
