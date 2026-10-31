// mapc: compile a JSON list of boxes and ramps into a Quake 3 BSP (IBSP v46).
//
//   mapc <input.json> <output.bsp>
//
// The output is deliberately simple: one BSP node whose children both point at
// a single leaf holding every brush and surface, one vis cluster that sees
// itself (everything is visible), and vertex lighting baked from an ambient term plus one sun.
// Collision tests every brush per trace, which is fine for test rooms and
// aim-trainer arenas with tens of brushes. Entities with their own brushes
// become submodels ("*1", "*2", ...); trigger_* ones are trigger volumes.
// See docs/mapc.md for the format.
//
// This file is part of Skaar. GPLv2 or later.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"

using json = nlohmann::json;

namespace {

// ---- on-disk structures (mirror code/qcommon/qfiles.h) ---------------------

constexpr int kBspVersion = 46;
constexpr int kNumLumps = 17;
constexpr int kContentsSolid = 1;
constexpr int kContentsTrigger = 0x40000000;  // CONTENTS_TRIGGER
constexpr int kSurfNoDraw = 0x80;             // SURF_NODRAW
constexpr const char* kTriggerShader = "textures/common/trigger";
constexpr int kSurfacePlanar = 1;  // MST_PLANAR
constexpr int kLightmapByVertex = -3;  // LIGHTMAP_BY_VERTEX (-1 would mean unlit-by-map, lit like a model)

// The renderer doubles map lighting (r_mapOverBrightBits 2 minus
// r_overBrightBits 1), so light values are stored at half intensity, as q3map2 does
constexpr float kOverbrightScale = 0.5f;

// renderergl2 defaults; a worldspawn "gridsize" key would override them
constexpr float kLightGridSize[3] = {64.0f, 64.0f, 128.0f};

enum Lump {
    kEntities, kShaders, kPlanes, kNodes, kLeafs, kLeafSurfaces, kLeafBrushes, kModels,
    kBrushes, kBrushSides, kDrawVerts, kDrawIndexes, kFogs, kSurfaces, kLightmaps,
    kLightGrid, kVisibility
};

struct LumpEntry { int32_t offset, length; };
struct Header { char ident[4]; int32_t version; LumpEntry lumps[kNumLumps]; };
struct DShader { char name[64]; int32_t surfaceFlags, contentFlags; };
struct DPlane { float normal[3]; float dist; };
struct DNode { int32_t planeNum; int32_t children[2]; int32_t mins[3], maxs[3]; };
struct DLeaf {
    int32_t cluster, area;
    int32_t mins[3], maxs[3];
    int32_t firstLeafSurface, numLeafSurfaces;
    int32_t firstLeafBrush, numLeafBrushes;
};
struct DModel { float mins[3], maxs[3]; int32_t firstSurface, numSurfaces, firstBrush, numBrushes; };
struct DBrushSide { int32_t planeNum, shaderNum; };
struct DBrush { int32_t firstSide, numSides, shaderNum; };
struct DrawVert { float xyz[3]; float st[2]; float lightmap[2]; float normal[3]; uint8_t color[4]; };
struct DSurface {
    int32_t shaderNum, fogNum, surfaceType;
    int32_t firstVert, numVerts, firstIndex, numIndexes;
    int32_t lightmapNum, lightmapX, lightmapY, lightmapWidth, lightmapHeight;
    float lightmapOrigin[3];
    float lightmapVecs[3][3];
    int32_t patchWidth, patchHeight;
};

static_assert(sizeof(Header) == 144);
static_assert(sizeof(DShader) == 72);
static_assert(sizeof(DPlane) == 16);
static_assert(sizeof(DNode) == 36);
static_assert(sizeof(DLeaf) == 48);
static_assert(sizeof(DModel) == 40);
static_assert(sizeof(DBrushSide) == 8);
static_assert(sizeof(DBrush) == 12);
static_assert(sizeof(DrawVert) == 44);
static_assert(sizeof(DSurface) == 104);

// ---- input -----------------------------------------------------------------

using Vec3 = std::array<float, 3>;

struct Plane3 {
    Vec3 normal;
    float dist;  // inside: dot(p, normal) < dist
};

// A convex brush: its axial bounds plus any non-axial sides (a ramp's slope).
struct Box {
    Vec3 min, max;
    std::string material;
    std::vector<Plane3> extra;
};

// An entity with its own brushes: becomes submodel "*N".
struct BrushEntity {
    size_t entity;  // index in the entities array
    bool trigger;   // trigger_*: CONTENTS_TRIGGER, nothing drawn
    std::vector<Box> boxes;
};

struct Light {
    float ambient = 0.45f;
    Vec3 sunDir = {0.35f, 0.25f, -1.0f};  // direction the light travels
    float sun = 0.55f;
};

// Every coordinate is rounded to int32 for the BSP and drives the light-grid
// size, so bound them here, at the one door floats come in through. The grid is
// 64x64x128 at 8 bytes a point, so +/-16384 (four times the largest shipped
// map) already asks for ~0.5 GB; the old +/-1e6 asked for 10^14 bytes and died
// in the allocator instead of reporting a bad map.
constexpr float kMaxCoord = 16384.0f;

// A room's wall thickness is added to those coordinates, so it needs the same
// bound: "wall": 3e9 put the room's bounds at +/-3e9 and overflowed int32 below.
constexpr float kMaxWall = 4096.0f;

Vec3 ReadVec3(const json& j, const char* what) {
    if (!j.is_array() || j.size() != 3) {
        throw std::runtime_error(std::string(what) + " must be an array of 3 numbers");
    }
    Vec3 out;
    for (int i = 0; i < 3; i++) {
        const float v = j[i].get<float>();
        if (!std::isfinite(v) || std::fabs(v) > kMaxCoord) {
            throw std::runtime_error(std::string(what) + " must be finite and within +/-16384");
        }
        out[i] = v;
    }
    return out;
}

std::string FormatVec3(const Vec3& v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%g %g %g", v[0], v[1], v[2]);
    return buf;
}

// A Q3 entity lump has no escape syntax: COM_ParseExt ends a quoted token at the
// next quote or newline, so either character inside a value mis-tokenises the
// whole lump and the game module drops the map. q3map2 substitutes, so do we.
std::string EntitySafe(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '"') {
            out.push_back('\'');
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// "room": hollow box built from six wall brushes around an interior volume
void AddRoom(const json& room, std::vector<Box>& boxes) {
    const Vec3 mn = ReadVec3(room.at("min"), "room.min");
    const Vec3 mx = ReadVec3(room.at("max"), "room.max");
    const float t = room.value("wall", 16.0f);
    const std::string walls = room.value("walls", std::string("wall"));
    const std::string floorMat = room.value("floor", walls);
    const std::string ceiling = room.value("ceiling", walls);

    // the walls are built from these directly, so ReadBox's guard never runs:
    // a swapped or zero-thickness room compiles to brushes that collide with
    // nothing and says nothing about it
    for (int i = 0; i < 3; i++) {
        if (mn[i] >= mx[i]) {
            throw std::runtime_error("room min must be < max on every axis");
        }
    }
    if (!std::isfinite(t) || t <= 0.0f || t > kMaxWall) {
        throw std::runtime_error("room wall must be finite, > 0 and within 4096");
    }

    boxes.push_back({{mn[0] - t, mn[1] - t, mn[2] - t}, {mx[0] + t, mx[1] + t, mn[2]}, floorMat, {}});
    boxes.push_back({{mn[0] - t, mn[1] - t, mx[2]}, {mx[0] + t, mx[1] + t, mx[2] + t}, ceiling, {}});
    boxes.push_back({{mn[0] - t, mn[1] - t, mn[2]}, {mn[0], mx[1] + t, mx[2]}, walls, {}});
    boxes.push_back({{mx[0], mn[1] - t, mn[2]}, {mx[0] + t, mx[1] + t, mx[2]}, walls, {}});
    boxes.push_back({{mn[0], mn[1] - t, mn[2]}, {mx[0], mn[1], mx[2]}, walls, {}});
    boxes.push_back({{mn[0], mx[1], mn[2]}, {mx[0], mx[1] + t, mx[2]}, walls, {}});
}

Box ReadBox(const json& b) {
    Box box{ReadVec3(b.at("min"), "brush.min"), ReadVec3(b.at("max"), "brush.max"),
            b.value("material", std::string("wall")), {}};
    for (int i = 0; i < 3; i++) {
        if (box.min[i] >= box.max[i]) {
            throw std::runtime_error("brush min must be < max on every axis");
        }
    }
    return box;
}

// "ramp": a wedge filling its box under a slope that rises from min.z on one
// side to max.z on the opposite side ("rise": "+x" climbs toward +x).
Box ReadRamp(const json& r) {
    Box box = ReadBox(r);
    const std::string rise = r.value("rise", std::string("+x"));
    if (rise.size() != 2 || (rise[0] != '+' && rise[0] != '-') || (rise[1] != 'x' && rise[1] != 'y')) {
        throw std::runtime_error("ramp.rise must be +x, -x, +y or -y");
    }
    const int axis = rise[1] == 'x' ? 0 : 1;
    const bool up = rise[0] == '+';
    const float run = box.max[axis] - box.min[axis];
    const float height = box.max[2] - box.min[2];

    // outward normal of the slope, pointing up and back down the slope
    Vec3 n = {0, 0, run};
    n[axis] = up ? -height : height;
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (float& c : n) {
        c /= len;
    }
    Vec3 low = box.min;  // a point on the low edge
    low[axis] = up ? box.min[axis] : box.max[axis];
    box.extra.push_back({n, n[0] * low[0] + n[1] * low[1] + n[2] * low[2]});
    return box;
}

void ReadSolids(const json& j, std::vector<Box>& out) {
    if (j.contains("brushes")) {
        for (const json& b : j.at("brushes")) {
            out.push_back(ReadBox(b));
        }
    }
    if (j.contains("ramps")) {
        for (const json& r : j.at("ramps")) {
            out.push_back(ReadRamp(r));
        }
    }
}

// Convex polygon on a plane, clipped by other planes (Sutherland-Hodgman).
using Polygon = std::vector<Vec3>;

Polygon BaseWinding(const Plane3& p) {
    // tangents t1, t2 with t1 x t2 = normal, so the corners below run
    // counter-clockwise seen from the front
    const Vec3& n = p.normal;
    Vec3 ref = std::fabs(n[2]) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    Vec3 t1 = {ref[1] * n[2] - ref[2] * n[1], ref[2] * n[0] - ref[0] * n[2], ref[0] * n[1] - ref[1] * n[0]};
    const float l = std::sqrt(t1[0] * t1[0] + t1[1] * t1[1] + t1[2] * t1[2]);
    for (float& c : t1) {
        c /= l;
    }
    const Vec3 t2 = {n[1] * t1[2] - n[2] * t1[1], n[2] * t1[0] - n[0] * t1[2], n[0] * t1[1] - n[1] * t1[0]};
    constexpr float kBig = 65536;
    Vec3 center;
    for (int i = 0; i < 3; i++) {
        center[i] = n[i] * p.dist;
    }
    Polygon w;
    const float signs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const auto& s : signs) {
        Vec3 v;
        for (int i = 0; i < 3; i++) {
            v[i] = center[i] + (s[0] * t1[i] + s[1] * t2[i]) * kBig;
        }
        w.push_back(v);
    }
    return w;
}

// Keeps the part of the polygon behind the plane (inside the brush).
Polygon Clip(const Polygon& in, const Plane3& p) {
    constexpr float kEpsilon = 0.01f;
    Polygon out;
    for (size_t i = 0; i < in.size(); i++) {
        const Vec3& a = in[i];
        const Vec3& b = in[(i + 1) % in.size()];
        const float da = a[0] * p.normal[0] + a[1] * p.normal[1] + a[2] * p.normal[2] - p.dist;
        const float db = b[0] * p.normal[0] + b[1] * p.normal[1] + b[2] * p.normal[2] - p.dist;
        if (da <= kEpsilon) {
            out.push_back(a);
        }
        if ((da < -kEpsilon && db > kEpsilon) || (da > kEpsilon && db < -kEpsilon)) {
            const float t = da / (da - db);
            Vec3 m;
            for (int k = 0; k < 3; k++) {
                m[k] = a[k] + t * (b[k] - a[k]);
            }
            out.push_back(m);
        }
    }
    return out;
}

float Area(const Polygon& w) {
    Vec3 sum = {0, 0, 0};
    for (size_t i = 1; i + 1 < w.size(); i++) {
        const Vec3 e1 = {w[i][0] - w[0][0], w[i][1] - w[0][1], w[i][2] - w[0][2]};
        const Vec3 e2 = {w[i + 1][0] - w[0][0], w[i + 1][1] - w[0][1], w[i + 1][2] - w[0][2]};
        sum[0] += e1[1] * e2[2] - e1[2] * e2[1];
        sum[1] += e1[2] * e2[0] - e1[0] * e2[2];
        sum[2] += e1[0] * e2[1] - e1[1] * e2[0];
    }
    return 0.5f * std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
}

// ---- compiler --------------------------------------------------------------

class Compiler {
public:
    void Load(const json& doc) {
        message_ = doc.value("message", doc.value("name", std::string("untitled")));

        if (doc.contains("materials")) {
            for (const auto& [key, value] : doc["materials"].items()) {
                materials_[key] = value.get<std::string>();
            }
        }
        if (doc.contains("light")) {
            const json& l = doc["light"];
            light_.ambient = l.value("ambient", light_.ambient);
            light_.sun = l.value("sun", light_.sun);
            if (l.contains("sunDir")) {
                light_.sunDir = ReadVec3(l["sunDir"], "light.sunDir");
            }
        }
        if (doc.contains("rooms")) {
            for (const json& room : doc["rooms"]) {
                AddRoom(room, boxes_);
            }
        }
        ReadSolids(doc, boxes_);
        if (boxes_.empty()) {
            throw std::runtime_error("map has no brushes or rooms");
        }
        if (doc.contains("entities")) {
            entities_ = doc["entities"];
            for (size_t i = 0; i < entities_.size(); i++) {
                const json& ent = entities_[i];
                BrushEntity be{i, ent.value("classname", std::string()).rfind("trigger_", 0) == 0, {}};
                ReadSolids(ent, be.boxes);
                if (!be.boxes.empty()) {
                    brushEntities_.push_back(std::move(be));
                }
            }
        }
    }

    std::vector<uint8_t> Compile() {
        BuildBrushesAndSurfaces();
        BuildTree();
        return Write();
    }

    size_t NumBrushes() const { return brushes_.size(); }
    size_t NumSurfaces() const { return surfaces_.size(); }

private:
    int ShaderFor(const std::string& material) {
        auto it = materials_.find(material);
        const std::string name = material == kTriggerShader ? material
                                 : it != materials_.end()   ? it->second
                                                            : "textures/skaar/" + material;
        for (size_t i = 0; i < shaders_.size(); i++) {
            if (name == shaders_[i].name) {
                return static_cast<int>(i);
            }
        }
        if (name.size() >= sizeof(DShader::name)) {
            throw std::runtime_error("material name too long: " + name);
        }
        DShader s{};
        std::memcpy(s.name, name.c_str(), name.size());
        s.contentFlags = kContentsSolid;
        if (name == kTriggerShader) {
            s.contentFlags = kContentsTrigger;
            s.surfaceFlags = kSurfNoDraw;
        }
        shaders_.push_back(s);
        return static_cast<int>(shaders_.size() - 1);
    }

    uint8_t Shade(const float n[3]) const {
        Vec3 d = light_.sunDir;
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float lambert = std::fmax(0.0f, -(n[0] * d[0] + n[1] * d[1] + n[2] * d[2]) / len);
        const float v = std::fmax(0.0f, std::fmin(1.0f, light_.ambient + light_.sun * lambert));
        return static_cast<uint8_t>(std::lround(v * 255.0f * kOverbrightScale));
    }

    void BuildBrushesAndSurfaces() {
        // the world first: model 0's brushes and surfaces must come first
        for (const Box& box : boxes_) {
            EmitBrush(box, ShaderFor(box.material), true);
            for (int i = 0; i < 3; i++) {
                mins_[i] = std::fmin(mins_[i], box.min[i]);
                maxs_[i] = std::fmax(maxs_[i], box.max[i]);
            }
        }
        numWorldBrushes_ = brushes_.size();
        numWorldSurfaces_ = surfaces_.size();

        // then one submodel per brush entity
        for (BrushEntity& be : brushEntities_) {
            DModel model{};
            model.firstBrush = static_cast<int32_t>(brushes_.size());
            model.firstSurface = static_cast<int32_t>(surfaces_.size());
            for (int i = 0; i < 3; i++) {
                model.mins[i] = 1e9f;
                model.maxs[i] = -1e9f;
            }
            for (const Box& box : be.boxes) {
                EmitBrush(box, ShaderFor(be.trigger ? kTriggerShader : box.material), !be.trigger);
                for (int i = 0; i < 3; i++) {
                    model.mins[i] = std::fmin(model.mins[i], box.min[i]);
                    model.maxs[i] = std::fmax(model.maxs[i], box.max[i]);
                }
            }
            model.numBrushes = static_cast<int32_t>(brushes_.size()) - model.firstBrush;
            model.numSurfaces = static_cast<int32_t>(surfaces_.size()) - model.firstSurface;
            submodels_.push_back(model);
        }
    }

    void EmitBrush(const Box& box, int shader, bool drawn) {
        DBrush brush{static_cast<int32_t>(sides_.size()), static_cast<int32_t>(6 + box.extra.size()), shader};

        // q3 convention: the first six sides are the axial planes in the
        // order -x, +x, -y, +y, -z, +z (CM_BoundBrush relies on it)
        std::vector<Plane3> planes;
        for (int axis = 0; axis < 3; axis++) {
            for (int positive = 0; positive < 2; positive++) {
                DPlane plane{};
                plane.normal[axis] = positive ? 1.0f : -1.0f;
                plane.dist = positive ? box.max[axis] : -box.min[axis];
                sides_.push_back({static_cast<int32_t>(planes_.size()), shader});
                planes_.push_back(plane);
                planes.push_back({{plane.normal[0], plane.normal[1], plane.normal[2]}, plane.dist});
                if (drawn && box.extra.empty()) {
                    AddFace(box, axis, positive != 0, shader);  // plain box: one quad per side
                }
            }
        }
        for (const Plane3& p : box.extra) {
            sides_.push_back({static_cast<int32_t>(planes_.size()), shader});
            planes_.push_back({{p.normal[0], p.normal[1], p.normal[2]}, p.dist});
            planes.push_back(p);
        }
        brushes_.push_back(brush);

        // other shapes: each side's polygon is a big quad on its plane clipped
        // by all the other sides; sides squeezed to nothing (a ramp's top and
        // low end) draw nothing
        if (drawn && !box.extra.empty()) {
            for (size_t i = 0; i < planes.size(); i++) {
                Polygon w = BaseWinding(planes[i]);
                for (size_t j = 0; j < planes.size() && w.size() >= 3; j++) {
                    if (j != i) {
                        w = Clip(w, planes[j]);
                    }
                }
                if (w.size() >= 3 && Area(w) > 1.0f) {
                    AddPolygon(w, planes[i].normal, shader);
                }
            }
        }
    }

    // A convex polygon (counter-clockwise seen from the front) as a triangle fan
    void AddPolygon(const Polygon& w, const Vec3& normalVec, int shader) {
        const float normal[3] = {normalVec[0], normalVec[1], normalVec[2]};
        DSurface surf{};
        surf.shaderNum = shader;
        surf.fogNum = -1;
        surf.surfaceType = kSurfacePlanar;
        surf.firstVert = static_cast<int32_t>(verts_.size());
        surf.numVerts = static_cast<int32_t>(w.size());
        surf.firstIndex = static_cast<int32_t>(indexes_.size());
        surf.numIndexes = static_cast<int32_t>((w.size() - 2) * 3);
        surf.lightmapNum = kLightmapByVertex;
        std::memcpy(surf.lightmapVecs[2], normal, sizeof(normal));

        // world-aligned texture mapping on the plane's dominant axis, like boxes
        int axis = 0;
        for (int i = 1; i < 3; i++) {
            if (std::fabs(normal[i]) > std::fabs(normal[axis])) {
                axis = i;
            }
        }
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const uint8_t shade = Shade(normal);
        for (const Vec3& p : w) {
            DrawVert vert{};
            for (int i = 0; i < 3; i++) {
                vert.xyz[i] = p[i];
            }
            vert.st[0] = p[u] / 128.0f;
            vert.st[1] = -p[v] / 128.0f;
            std::memcpy(vert.normal, normal, sizeof(normal));
            vert.color[0] = vert.color[1] = vert.color[2] = shade;
            vert.color[3] = 255;
            verts_.push_back(vert);
        }
        // q3 renders clockwise-from-the-front triangles
        for (int32_t i = 1; i + 1 < surf.numVerts; i++) {
            indexes_.push_back(0);
            indexes_.push_back(i + 1);
            indexes_.push_back(i);
        }
        surfaces_.push_back(surf);
    }

    // One quad per brush side, facing out of the brush
    void AddFace(const Box& box, int axis, bool positive, int shader) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const float w = positive ? box.max[axis] : box.min[axis];
        float normal[3] = {0, 0, 0};
        normal[axis] = positive ? 1.0f : -1.0f;

        DSurface surf{};
        surf.shaderNum = shader;
        surf.fogNum = -1;
        surf.surfaceType = kSurfacePlanar;
        surf.firstVert = static_cast<int32_t>(verts_.size());
        surf.numVerts = 4;
        surf.firstIndex = static_cast<int32_t>(indexes_.size());
        surf.numIndexes = 6;
        surf.lightmapNum = kLightmapByVertex;
        std::memcpy(surf.lightmapVecs[2], normal, sizeof(normal));

        const uint8_t shade = Shade(normal);
        const float corners[4][2] = {
            {box.min[u], box.min[v]}, {box.max[u], box.min[v]},
            {box.max[u], box.max[v]}, {box.min[u], box.max[v]},
        };
        for (const auto& c : corners) {
            DrawVert vert{};
            vert.xyz[axis] = w;
            vert.xyz[u] = c[0];
            vert.xyz[v] = c[1];
            // world-aligned texture mapping: one texture repeat per 128 units
            vert.st[0] = c[0] / 128.0f;
            vert.st[1] = -c[1] / 128.0f;
            std::memcpy(vert.normal, normal, sizeof(normal));
            vert.color[0] = vert.color[1] = vert.color[2] = shade;
            vert.color[3] = 255;
            verts_.push_back(vert);
        }

        // q3 renders clockwise-from-the-front triangles; (u, v, axis) is
        // right-handed, so corners run counter-clockwise seen from +axis
        static const int kCcw[6] = {0, 1, 2, 0, 2, 3};
        static const int kCw[6] = {0, 2, 1, 0, 3, 2};
        const int* order = positive ? kCw : kCcw;
        for (int i = 0; i < 6; i++) {
            indexes_.push_back(order[i]);
        }
        surfaces_.push_back(surf);
    }

    void BuildTree() {
        // Root node: any plane will do, both children are the single leaf
        DNode node{};
        node.planeNum = 0;
        node.children[0] = node.children[1] = -1;  // -1 - leaf 0

        DLeaf leaf{};
        leaf.cluster = 0;
        leaf.area = 0;
        leaf.firstLeafSurface = 0;
        leaf.numLeafSurfaces = static_cast<int32_t>(numWorldSurfaces_);
        leaf.firstLeafBrush = 0;
        leaf.numLeafBrushes = static_cast<int32_t>(numWorldBrushes_);

        for (int i = 0; i < 3; i++) {
            node.mins[i] = leaf.mins[i] = static_cast<int32_t>(std::floor(mins_[i])) - 1;
            node.maxs[i] = leaf.maxs[i] = static_cast<int32_t>(std::ceil(maxs_[i])) + 1;
        }
        nodes_.push_back(node);
        leafs_.push_back(leaf);

        // submodels aren't in the world leaf: the collision model gives each
        // one its own leaf, and the renderer draws them through their entity
        for (size_t i = 0; i < numWorldSurfaces_; i++) {
            leafSurfaces_.push_back(static_cast<int32_t>(i));
        }
        for (size_t i = 0; i < numWorldBrushes_; i++) {
            leafBrushes_.push_back(static_cast<int32_t>(i));
        }

        DModel world{};
        for (int i = 0; i < 3; i++) {
            world.mins[i] = mins_[i];
            world.maxs[i] = maxs_[i];
        }
        world.numSurfaces = static_cast<int32_t>(numWorldSurfaces_);
        world.numBrushes = static_cast<int32_t>(numWorldBrushes_);
        models_.push_back(world);
        models_.insert(models_.end(), submodels_.begin(), submodels_.end());

        BuildLightGrid();
    }

    // Must match R_LoadLightGrid's sizing exactly, or the renderer rejects it
    void BuildLightGrid() {
        int bounds[3];
        for (int i = 0; i < 3; i++) {
            const float origin = kLightGridSize[i] * std::ceil(mins_[i] / kLightGridSize[i]);
            const float max = kLightGridSize[i] * std::floor(maxs_[i] / kLightGridSize[i]);
            bounds[i] = static_cast<int>((max - origin) / kLightGridSize[i]) + 1;
        }

        // direction towards the light, encoded as the renderer decodes it:
        // x = cos(lat) sin(lng), y = sin(lat) sin(lng), z = cos(lng)
        Vec3 d = light_.sunDir;
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float tx = -d[0] / len, ty = -d[1] / len, tz = -d[2] / len;
        const float twoPi = 6.28318530718f;
        float lat = std::atan2(ty, tx);
        if (lat < 0) {
            lat += twoPi;
        }
        const float lng = std::acos(std::fmax(-1.0f, std::fmin(1.0f, tz)));

        const auto byte = [](float v) {
            // a negative light would wrap through uint8_t into the bright half
            return static_cast<uint8_t>(std::lround(std::fmax(0.0f, std::fmin(1.0f, v)) * 255.0f * kOverbrightScale));
        };
        const uint8_t point[8] = {
            byte(light_.ambient), byte(light_.ambient), byte(light_.ambient),
            byte(light_.sun), byte(light_.sun), byte(light_.sun),
            static_cast<uint8_t>(std::lround(lng / twoPi * 256.0f) & 255),
            static_cast<uint8_t>(std::lround(lat / twoPi * 256.0f) & 255),
        };

        const size_t numPoints = static_cast<size_t>(bounds[0]) * bounds[1] * bounds[2];
        lightGrid_.reserve(numPoints * 8);
        for (size_t i = 0; i < numPoints; i++) {
            lightGrid_.insert(lightGrid_.end(), point, point + 8);
        }
    }

    std::string EntityString() const {
        std::string out = "{\n\"classname\" \"worldspawn\"\n\"message\" \"" + EntitySafe(message_) + "\"\n}\n";
        for (size_t e = 0; e < entities_.size(); e++) {
            const json& ent = entities_[e];
            out += "{\n";
            for (size_t m = 0; m < brushEntities_.size(); m++) {
                if (brushEntities_[m].entity == e) {
                    out += "\"model\" \"*" + std::to_string(m + 1) + "\"\n";
                }
            }
            for (const auto& [key, value] : ent.items()) {
                if (key == "brushes" || key == "ramps") {
                    continue;  // compiled into the submodel
                }
                std::string text;
                if (value.is_string()) {
                    text = value.get<std::string>();
                } else if (value.is_array()) {
                    text = FormatVec3(ReadVec3(value, key.c_str()));
                } else {
                    text = value.dump();
                }
                out += "\"" + EntitySafe(key) + "\" \"" + EntitySafe(text) + "\"\n";
            }
            out += "}\n";
        }
        return out;
    }

    template <typename T>
    static void Append(std::vector<uint8_t>& out, Header& header, Lump lump, const T* data, size_t count) {
        while (out.size() % 4) {
            out.push_back(0);
        }
        header.lumps[lump].offset = static_cast<int32_t>(out.size());
        header.lumps[lump].length = static_cast<int32_t>(count * sizeof(T));
        const auto* bytes = reinterpret_cast<const uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + count * sizeof(T));
    }

    std::vector<uint8_t> Write() const {
        std::vector<uint8_t> out(sizeof(Header), 0);
        Header header{};
        std::memcpy(header.ident, "IBSP", 4);
        header.version = kBspVersion;

        const std::string ents = EntityString();
        Append(out, header, kEntities, ents.c_str(), ents.size() + 1);
        Append(out, header, kShaders, shaders_.data(), shaders_.size());
        Append(out, header, kPlanes, planes_.data(), planes_.size());
        Append(out, header, kNodes, nodes_.data(), nodes_.size());
        Append(out, header, kLeafs, leafs_.data(), leafs_.size());
        Append(out, header, kLeafSurfaces, leafSurfaces_.data(), leafSurfaces_.size());
        Append(out, header, kLeafBrushes, leafBrushes_.data(), leafBrushes_.size());
        Append(out, header, kModels, models_.data(), models_.size());
        Append(out, header, kBrushes, brushes_.data(), brushes_.size());
        Append(out, header, kBrushSides, sides_.data(), sides_.size());
        Append(out, header, kDrawVerts, verts_.data(), verts_.size());
        Append(out, header, kDrawIndexes, indexes_.data(), indexes_.size());
        Append<uint8_t>(out, header, kFogs, nullptr, 0);
        Append(out, header, kSurfaces, surfaces_.data(), surfaces_.size());
        Append<uint8_t>(out, header, kLightmaps, nullptr, 0);
        Append(out, header, kLightGrid, lightGrid_.data(), lightGrid_.size());
        // The renderer skips leafs whose cluster >= numClusters, so even a
        // one-leaf map needs a vis lump: numClusters, clusterBytes, then one
        // row per cluster (cluster 0 sees itself)
        const int32_t vis[3] = {1, 4, 1};
        Append(out, header, kVisibility, vis, 3);

        std::memcpy(out.data(), &header, sizeof(header));
        return out;
    }

    std::string message_;
    std::map<std::string, std::string> materials_;
    Light light_;
    std::vector<Box> boxes_;
    json entities_ = json::array();
    std::vector<BrushEntity> brushEntities_;
    std::vector<DModel> submodels_;
    size_t numWorldBrushes_ = 0;
    size_t numWorldSurfaces_ = 0;

    Vec3 mins_ = {1e9f, 1e9f, 1e9f};
    Vec3 maxs_ = {-1e9f, -1e9f, -1e9f};

    std::vector<DShader> shaders_;
    std::vector<DPlane> planes_;
    std::vector<DNode> nodes_;
    std::vector<DLeaf> leafs_;
    std::vector<int32_t> leafSurfaces_;
    std::vector<int32_t> leafBrushes_;
    std::vector<DModel> models_;
    std::vector<DBrush> brushes_;
    std::vector<DBrushSide> sides_;
    std::vector<DrawVert> verts_;
    std::vector<int32_t> indexes_;
    std::vector<DSurface> surfaces_;
    std::vector<uint8_t> lightGrid_;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: mapc <input.json> <output.bsp>\n");
        return 2;
    }

    try {
        std::ifstream in(argv[1]);
        if (!in) {
            throw std::runtime_error("can't open input");
        }
        const json doc = json::parse(in, nullptr, true, true);  // allow comments

        Compiler compiler;
        compiler.Load(doc);
        const std::vector<uint8_t> bsp = compiler.Compile();

        std::ofstream out(argv[2], std::ios::binary);
        if (!out) {
            throw std::runtime_error(std::string("can't write ") + argv[2]);
        }
        out.write(reinterpret_cast<const char*>(bsp.data()), static_cast<std::streamsize>(bsp.size()));
        std::printf("mapc: %s -> %s (%zu brushes, %zu surfaces, %zu bytes)\n", argv[1], argv[2],
                    compiler.NumBrushes(), compiler.NumSurfaces(), bsp.size());
    } catch (const std::exception& e) {
        // file:line style so the dev-watch output and toasts point at the source
        std::fprintf(stderr, "%s:1: error: mapc: %s\n", argv[1], e.what());
        return 1;
    }
    return 0;
}
