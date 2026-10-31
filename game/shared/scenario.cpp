// This file is part of Skaar. GPLv2 or later.
#include "scenario.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "json.hpp"

namespace skaar {
namespace {

using json = nlohmann::json;

// a map name is a file stem under maps/, and the search for one ignores case
constexpr char kMapChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
constexpr std::size_t kMaxMapName = 54;  // MAX_QPATH - strlen("maps/") - strlen(".bsp") - 1

// A JSON number as a float. Checked while it is still a double: narrowing an
// out-of-float-range value is UB, and the value would only be rejected later.
float Number(const json& v, const char* key) {
    if (!v.is_number()) {
        throw std::runtime_error(std::string(key) + " must be a number");
    }
    const double d = v.get<double>();
    if (!std::isfinite(d) || std::fabs(d) > static_cast<double>(std::numeric_limits<float>::max())) {
        throw std::runtime_error(std::string(key) + " is out of range");
    }
    return static_cast<float>(d);
}

Range ReadRange(const json& j, const char* key, Range fallback) {
    if (!j.contains(key)) {
        return fallback;
    }
    const json& v = j.at(key);
    if (v.is_number()) {
        const float f = Number(v, key);
        return {f, f};
    }
    if (v.is_array() && v.size() == 2) {
        Range r{Number(v[0], key), Number(v[1], key)};
        if (r.min > r.max) {
            std::swap(r.min, r.max);
        }
        return r;
    }
    throw std::runtime_error(std::string(key) + " must be a number or [min, max]");
}

template <typename T>
T Get(const json& j, const char* key, T fallback) {
    if (!j.contains(key)) {
        return fallback;
    }
    const json& v = j.at(key);
    if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(Number(v, key));
    } else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        if (!v.is_number()) {
            throw std::runtime_error(std::string(key) + " must be a number");
        }
        // get<int>() narrows with no range test: UB for an out-of-range number and
        // a silent truncation for a fractional one. A float is still a whole number
        // when it is exactly integral -- a hand-written file spells 4 as 4.0 and
        // nlohmann used to truncate it -- so only a genuine fraction is rejected.
        const double d = v.get<double>();
        if (!std::isfinite(d) || std::trunc(d) != d) {
            throw std::runtime_error(std::string(key) + " must be a whole number");
        }
        if (d < static_cast<double>(std::numeric_limits<T>::min()) ||
            d > static_cast<double>(std::numeric_limits<T>::max())) {
            throw std::runtime_error(std::string(key) + " is out of range");
        }
        return static_cast<T>(d);
    } else {
        return v.get<T>();
    }
}

json ParseJson(std::string_view text) {
    // comments allowed, like the map sources
    return json::parse(text.begin(), text.end(), nullptr, true, true);
}

}  // namespace

bool ParseScenario(std::string_view id, std::string_view text, Scenario& out, std::string& error) {
    try {
        const json j = ParseJson(text);
        if (!j.is_object()) {
            throw std::runtime_error("a scenario must be a JSON object");
        }
        Scenario s;
        s.id = id;
        s.name = Get<std::string>(j, "name", s.id);
        s.category = Get<std::string>(j, "category", "click");
        s.description = Get<std::string>(j, "description", "");
        s.map = Get<std::string>(j, "map", s.map);
        // it ends up in a console command ("map %s"): keep it to a plain map name.
        // Mixed case is allowed, like the map search itself and the community maps
        // that are named Ctf_Range; the length is bounded so that the engine's
        // "maps/<name>.bsp" still fits MAX_QPATH (64): 5 + name + 4 + the terminator
        if (s.map.empty() || s.map.size() > kMaxMapName ||
            s.map.find_first_not_of(kMapChars) != std::string::npos) {
            throw std::runtime_error("map must be a map name like \"range\"");
        }
        s.duration = Get<float>(j, "duration", s.duration);
        s.seed = Get<uint32_t>(j, "seed", 0);

        if (j.contains("player")) {
            const json& player = j.at("player");
            if (!player.is_object()) {
                throw std::runtime_error("player must be an object");
            }
            s.playerMove = Get<bool>(player, "move", false);
            s.ruleset = Get<std::string>(player, "ruleset", s.ruleset);
            // it ends up in a cvar and a file name: keep it to a plain id
            if (s.ruleset.empty() || s.ruleset.size() > 32 ||
                s.ruleset.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) {
                throw std::runtime_error("player.ruleset must be a lowercase id like \"cpm\"");
            }
        }

        const std::string fire = Get<std::string>(j, "fire", "click");
        if (fire == "click") {
            s.fire = FireMode::kClick;
        } else if (fire == "hold") {
            s.fire = FireMode::kHold;
        } else {
            throw std::runtime_error("fire must be \"click\" or \"hold\"");
        }

        s.weapon = Get<std::string>(j, "weapon", "");
        if (s.weapon.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) {
            throw std::runtime_error("weapon must be a lowercase id like \"rifle\"");
        }

        if (j.contains("targets")) {
            const json& t = j.at("targets");
            if (!t.is_object()) {
                throw std::runtime_error("targets must be an object");
            }
            auto& o = s.targets;
            o.alive = Get<int>(t, "alive", o.alive);
            o.radius = Get<float>(t, "radius", o.radius);
            o.health = Get<float>(t, "health", o.health);
            o.lifetime = Get<float>(t, "lifetime", o.lifetime);

            if (t.contains("spawn")) {
                const json& sp = t.at("spawn");
                if (!sp.is_object()) {
                    throw std::runtime_error("targets.spawn must be an object");
                }
                const std::string shape = Get<std::string>(sp, "shape", "cone");
                if (shape == "cone") {
                    o.shape = SpawnShape::kCone;
                } else if (shape == "grid") {
                    o.shape = SpawnShape::kGrid;
                } else {
                    throw std::runtime_error("spawn.shape must be \"cone\" or \"grid\"");
                }
                o.distance = ReadRange(sp, "distance", o.distance);
                o.yaw = ReadRange(sp, "yaw", o.yaw);
                o.pitch = ReadRange(sp, "pitch", o.pitch);
                o.cols = Get<int>(sp, "cols", o.cols);
                o.rows = Get<int>(sp, "rows", o.rows);
            }
            if (t.contains("motion")) {
                const json& m = t.at("motion");
                if (!m.is_object()) {
                    throw std::runtime_error("targets.motion must be an object");
                }
                const std::string type = Get<std::string>(m, "type", "static");
                if (type == "static") {
                    o.motion = MotionType::kStatic;
                } else if (type == "strafe") {
                    o.motion = MotionType::kStrafe;
                } else if (type == "float") {
                    o.motion = MotionType::kFloat;
                } else {
                    throw std::runtime_error("motion.type must be static, strafe or float");
                }
                o.speed = ReadRange(m, "speed", o.speed);
                o.changeEvery = ReadRange(m, "changeEvery", o.changeEvery);
            }
        }

        if (j.contains("score")) {
            const json& sc = j.at("score");
            if (!sc.is_object()) {
                throw std::runtime_error("score must be an object");
            }
            s.score.kill = Get<float>(sc, "kill", s.score.kill);
            s.score.accuracyWeight = Get<float>(sc, "accuracyWeight", s.score.accuracyWeight);
            s.score.perSecondOnTarget = Get<float>(sc, "perSecondOnTarget", s.score.perSecondOnTarget);
        }

        // sanity limits: bad data must not hang or crash the sim
        if (s.duration < 5 || s.duration > 600) {
            throw std::runtime_error("duration must be 5..600 seconds");
        }
        if (s.targets.alive < 1 || s.targets.alive > 16) {
            throw std::runtime_error("targets.alive must be 1..16");
        }
        if (s.targets.radius < 1 || s.targets.radius > 256) {
            throw std::runtime_error("targets.radius must be 1..256");
        }
        if (s.targets.health <= 0 || s.targets.health > 100000) {
            throw std::runtime_error("targets.health must be > 0 and <= 100000");
        }
        if (s.targets.distance.min < 64 || s.targets.distance.max > 100000) {
            throw std::runtime_error("targets.spawn.distance must be 64..100000");
        }
        if (s.targets.cols < 1 || s.targets.rows < 1 ||
            static_cast<int64_t>(s.targets.cols) * s.targets.rows > 256) {
            throw std::runtime_error("targets.spawn cols/rows out of range");
        }
        // widened too: the product of two accepted values is then <= 256, so the sim's
        // int `cells = cols * rows` cannot overflow either
        if (s.targets.shape == SpawnShape::kGrid &&
            static_cast<int64_t>(s.targets.cols) * s.targets.rows <= s.targets.alive) {
            throw std::runtime_error("a grid needs more cells than targets.alive");
        }
        if (s.targets.changeEvery.min < 0.05f || s.targets.changeEvery.max > 60) {
            throw std::runtime_error("motion.changeEvery must be 0.05..60 seconds");
        }
        // speed in world units per second: unbounded, a target crosses its whole window
        // between two Advance() calls, and how far it gets then depends on the frame rate
        if (s.targets.speed.min < -5000 || s.targets.speed.max > 5000) {
            throw std::runtime_error("motion.speed must be within -5000..5000 units per second");
        }
        if (!(s.score.accuracyWeight >= 0 && s.score.accuracyWeight <= 1)) {
            throw std::runtime_error("score.accuracyWeight must be 0..1");
        }
        if (s.score.kill < 0 || s.score.perSecondOnTarget < 0) {
            throw std::runtime_error("score.kill and score.perSecondOnTarget must be >= 0");
        }
        if (s.score.kill > 1e7f || s.score.perSecondOnTarget > 1e7f) {
            throw std::runtime_error("score.kill and score.perSecondOnTarget must be <= 10000000");
        }
        if (!(s.targets.lifetime >= 0) ||  // also rejects NaN
            (s.targets.lifetime > 0 && s.targets.lifetime < 0.05f)) {
            // a tiny lifetime stalls the respawn loop: spawnT + lifetime stops advancing
            throw std::runtime_error("targets.lifetime must be 0 (never) or >= 0.05 seconds");
        }

        out = std::move(s);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::string ScenarioToJson(const Scenario& s) {
    auto range = [](Range r) { return r.min == r.max ? json(r.min) : json::array({r.min, r.max}); };
    const auto& t = s.targets;
    // every field is written, so ParseScenario(ScenarioToJson(s)) gives s back
    json spawn = {{"shape", t.shape == SpawnShape::kGrid ? "grid" : "cone"},
                  {"distance", range(t.distance)},
                  {"yaw", range(t.yaw)},
                  {"pitch", range(t.pitch)},
                  {"cols", t.cols},
                  {"rows", t.rows}};
    const char* motion = "static";
    if (t.motion == MotionType::kStrafe) {
        motion = "strafe";
    } else if (t.motion == MotionType::kFloat) {
        motion = "float";
    }
    json motionBlock = {{"type", motion}, {"speed", range(t.speed)}, {"changeEvery", range(t.changeEvery)}};
    json targets = {{"alive", t.alive}, {"radius", t.radius}, {"health", t.health}, {"lifetime", t.lifetime},
                    {"spawn", spawn}, {"motion", motionBlock}};
    json j = {
        {"name", s.name},
        {"category", s.category},
        {"description", s.description},
        {"map", s.map},
        {"duration", s.duration},
        {"player", {{"move", s.playerMove}, {"ruleset", s.ruleset}}},
        {"fire", s.fire == FireMode::kHold ? "hold" : "click"},
        {"targets", targets},
        {"score",
         {{"kill", s.score.kill},
          {"accuracyWeight", s.score.accuracyWeight},
          {"perSecondOnTarget", s.score.perSecondOnTarget}}},
    };
    if (s.seed) {
        j["seed"] = s.seed;
    }
    if (!s.weapon.empty()) {
        j["weapon"] = s.weapon;
    }
    // a scenario id or name can come from a pack file name, which need not be UTF-8:
    // replace the bytes rather than throw out of the ui's save path
    return j.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

bool ParseBenchmark(std::string_view id, std::string_view text, Benchmark& out, std::string& error) {
    try {
        const json j = ParseJson(text);
        Benchmark b;
        b.id = id;
        b.name = Get<std::string>(j, "name", b.id);
        b.description = Get<std::string>(j, "description", "");
        b.tiers = j.at("tiers").get<std::vector<std::string>>();
        if (b.tiers.empty()) {
            throw std::runtime_error("tiers is empty");
        }
        for (const json& e : j.at("scenarios")) {
            Benchmark::Entry entry;
            entry.scenario = e.at("id").get<std::string>();
            const json& thresholds = e.at("thresholds");
            if (!thresholds.is_array() || thresholds.size() != b.tiers.size()) {
                throw std::runtime_error(entry.scenario + ": thresholds must match tiers");
            }
            for (const json& t : thresholds) {
                entry.thresholds.push_back(Number(t, "threshold"));
            }
            if (!std::is_sorted(entry.thresholds.begin(), entry.thresholds.end())) {
                throw std::runtime_error(entry.scenario + ": thresholds must ascend");
            }
            b.entries.push_back(std::move(entry));
        }
        out = std::move(b);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

int TierFor(const Benchmark::Entry& entry, float score, float* progress) {
    const auto& t = entry.thresholds;
    int tier = -1;
    for (size_t i = 0; i < t.size(); i++) {
        if (score >= t[i]) {
            tier = static_cast<int>(i);
        }
    }
    if (progress) {
        if (tier + 1 >= static_cast<int>(t.size())) {
            *progress = 1;
        } else {
            const float lo = tier < 0 ? 0 : t[tier];
            const float hi = t[tier + 1];
            *progress = hi > lo ? std::clamp((score - lo) / (hi - lo), 0.0f, 1.0f) : 0;
        }
    }
    return tier;
}

}  // namespace skaar
