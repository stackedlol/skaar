// This file is part of Skaar. GPLv2 or later.
#include "move_ruleset.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>

#include "json.hpp"
#include "q3.h"

namespace skaar::move {
namespace {

using json = nlohmann::json;

Ruleset MakeVq3() {
    Ruleset r;
    r.id = "vq3";
    r.label = "Classic";
    return r;
}

// values and their sources: docs/cpm-physics.md
Ruleset MakeCpm() {
    Ruleset r = MakeVq3();
    r.id = "cpm";
    r.label = "Advanced";
    r.accelerate = 15;
    r.airStopAccelerate = 2.5f;
    r.strafeAccelerate = 70;
    r.strafeWishSpeed = 30;
    r.airControl = 150;
    r.rampJump = true;
    r.doubleJumpWindowMs = 400;
    r.doubleJumpBoost = 100;
    return r;
}

std::vector<Ruleset>& Registry() {
    static std::vector<Ruleset> rulesets{MakeVq3(), MakeCpm()};
    return rulesets;
}

void Read(const json& j, const char* key, float& value, float min, float max) {
    if (!j.contains(key)) {
        return;
    }
    // the JSON number is checked as a double: narrowing an out-of-float-range
    // value first is UB, and the range test would then only see the narrowing
    const double d = j.at(key).get<double>();
    if (!(d >= min && d <= max)) {  // also rejects NaN
        throw std::runtime_error(std::string(key) + " must be between " + std::to_string(min) + " and " +
                                 std::to_string(max));
    }
    const float v = static_cast<float>(d);
    // %.9g only round-trips normal floats: a subnormal encodes to a decimal that the
    // client cannot decode, and it used to lose the whole ruleset
    if (v != 0 && std::fpclassify(v) == FP_SUBNORMAL) {
        throw std::runtime_error(std::string(key) + " is too small to send to clients");
    }
    value = v;
}

void Read(const json& j, const char* key, int& value, int min, int max) {
    if (!j.contains(key)) {
        return;
    }
    // widen first: get<int>() narrows before any range test, which is UB for an
    // out-of-range number and truncates 12.7 to 12 behind the user's back
    if (!j.at(key).is_number_integer()) {
        throw std::runtime_error(std::string(key) + " must be a whole number");
    }
    const int64_t v = j.at(key).get<int64_t>();
    if (v < min || v > max) {
        throw std::runtime_error(std::string(key) + " must be between " + std::to_string(min) + " and " +
                                 std::to_string(max));
    }
    value = static_cast<int>(v);
}

void Read(const json& j, const char* key, bool& value) {
    if (j.contains(key)) {
        value = j.at(key).get<bool>();
    }
}

}  // namespace

const Ruleset& Vq3() {
    static const Ruleset r = MakeVq3();
    return r;
}

const Ruleset& Cpm() {
    static const Ruleset r = MakeCpm();
    return r;
}

namespace {

// Builds a ruleset from a parsed object; throws on bad values.
Ruleset FromObject(std::string_view id, const json& j) {
    if (!j.is_object()) {
        throw std::runtime_error("expected a JSON object");
    }
    Ruleset r = id == Cpm().id ? Cpm() : Vq3();
    r.id = std::string(id);
    if (j.contains("label")) {
        r.label = j.at("label").get<std::string>();
    } else if (id != Vq3().id && id != Cpm().id) {
        r.label = r.id;
    }

    Read(j, "maxSpeed", r.maxSpeed, 0, 2000);
    Read(j, "accelerate", r.accelerate, 0, 100);
    Read(j, "friction", r.friction, 0, 100);
    Read(j, "stopSpeed", r.stopSpeed, 0, 1000);
    Read(j, "duckScale", r.duckScale, 0, 1);

    Read(j, "airAccelerate", r.airAccelerate, 0, 100);
    Read(j, "airStopAccelerate", r.airStopAccelerate, 0, 100);
    Read(j, "strafeAccelerate", r.strafeAccelerate, 0, 1000);
    Read(j, "strafeWishSpeed", r.strafeWishSpeed, 0, 1000);
    Read(j, "airControl", r.airControl, 0, 1000);
    Read(j, "airControlPower", r.airControlPower, 0, 8);
    Read(j, "airControlBackward", r.airControlBackward);
    Read(j, "airSpeedCap", r.airSpeedCap, 0, 2000);

    Read(j, "jumpVelocity", r.jumpVelocity, 0, 2000);
    Read(j, "rampJump", r.rampJump);
    Read(j, "doubleJumpWindowMs", r.doubleJumpWindowMs, 0, 2000);
    Read(j, "doubleJumpBoost", r.doubleJumpBoost, 0, 2000);
    Read(j, "autoHop", r.autoHop);

    Read(j, "swimScale", r.swimScale, 0, 1);
    Read(j, "waterAccelerate", r.waterAccelerate, 0, 100);
    Read(j, "waterFriction", r.waterFriction, 0, 100);
    Read(j, "flyAccelerate", r.flyAccelerate, 0, 100);
    Read(j, "flightFriction", r.flightFriction, 0, 100);
    Read(j, "spectatorFriction", r.spectatorFriction, 0, 100);
    return r;
}

}  // namespace

bool ParseRuleset(std::string_view id, std::string_view text, Ruleset& out, std::string& error) {
    try {
        // comments allowed, like the scenario and map sources
        out = FromObject(id, json::parse(text.begin(), text.end(), nullptr, true, true));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

void ResetRulesets() {
    Registry() = {MakeVq3(), MakeCpm()};
}

int AddRuleset(const Ruleset& ruleset) {
    std::vector<Ruleset>& all = Registry();
    const int existing = FindRuleset(ruleset.id);
    if (existing >= 0) {
        all[static_cast<size_t>(existing)] = ruleset;
        return existing;
    }
    all.push_back(ruleset);
    return static_cast<int>(all.size()) - 1;
}

int FindRuleset(std::string_view id) {
    const std::vector<Ruleset>& all = Registry();
    for (size_t i = 0; i < all.size(); i++) {
        if (all[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int RulesetCount() {
    return static_cast<int>(Registry().size());
}

const Ruleset& RulesetAt(int index) {
    const std::vector<Ruleset>& all = Registry();
    if (index < 0 || index >= static_cast<int>(all.size())) {
        return all.front();
    }
    return all[static_cast<size_t>(index)];
}

void SetRulesetAt(int index, const Ruleset& ruleset) {
    if (index < 0 || index > 255) {
        return;
    }
    std::vector<Ruleset>& all = Registry();
    while (static_cast<int>(all.size()) <= index) {
        all.push_back(MakeVq3());
    }
    all[static_cast<size_t>(index)] = ruleset;
}

void LoadRulesets(const std::function<std::vector<std::string>(const char* dir, const char* ext)>& list,
                  const std::function<bool(const std::string& path, std::string& text)>& read,
                  std::vector<std::string>& errors) {
    ResetRulesets();
    std::vector<std::string> files = list("rulesets", ".json");
    std::sort(files.begin(), files.end());
    for (const std::string& file : files) {
        if (file.size() <= 5) {
            continue;
        }
        std::string text;
        if (!read("rulesets/" + file, text)) {
            errors.push_back(file + ": can't read");
            continue;
        }
        Ruleset r;
        std::string error;
        if (!ParseRuleset(std::string_view(file).substr(0, file.size() - 5), text, r, error)) {
            errors.push_back(file + ": " + error);
            continue;
        }
        AddRuleset(r);
    }
}

std::string ToJson(const Ruleset& r) {
    json j;
    j["label"] = r.label;
    j["maxSpeed"] = r.maxSpeed;
    j["accelerate"] = r.accelerate;
    j["friction"] = r.friction;
    j["stopSpeed"] = r.stopSpeed;
    j["duckScale"] = r.duckScale;
    j["airAccelerate"] = r.airAccelerate;
    j["airStopAccelerate"] = r.airStopAccelerate;
    j["strafeAccelerate"] = r.strafeAccelerate;
    j["strafeWishSpeed"] = r.strafeWishSpeed;
    j["airControl"] = r.airControl;
    j["airControlPower"] = r.airControlPower;
    j["airControlBackward"] = r.airControlBackward;
    j["airSpeedCap"] = r.airSpeedCap;
    j["jumpVelocity"] = r.jumpVelocity;
    j["rampJump"] = r.rampJump;
    j["doubleJumpWindowMs"] = r.doubleJumpWindowMs;
    j["doubleJumpBoost"] = r.doubleJumpBoost;
    j["autoHop"] = r.autoHop;
    j["swimScale"] = r.swimScale;
    j["waterAccelerate"] = r.waterAccelerate;
    j["waterFriction"] = r.waterFriction;
    j["flyAccelerate"] = r.flyAccelerate;
    j["flightFriction"] = r.flightFriction;
    j["spectatorFriction"] = r.spectatorFriction;
    return j.dump();
}

// The configstring is a Q3 info string (\\key\\value), not JSON: configstring
// updates travel as a quoted "cs" server command, which would eat JSON's quotes.
std::string EncodeRuleset(int index) {
    const Ruleset& r = RulesetAt(index);
    char info[BIG_INFO_STRING] = "";
    auto set = [&info](const char* key, std::string value) {
        for (char& c : value) {
            if (c == '\\' || c == ';' || c == '"') {
                c = ' ';  // not allowed in info strings
            }
        }
        Info_SetValueForKey_Big(info, key, value.c_str());
    };
    set("index", std::to_string(index));
    set("id", r.id);
    const json fields = json::parse(ToJson(r));  // named: items() doesn't keep a temporary alive
    for (const auto& [key, value] : fields.items()) {
        if (value.is_string()) {
            set(key.c_str(), value.get<std::string>());
        } else if (value.is_boolean()) {
            set(key.c_str(), value.get<bool>() ? "1" : "0");
        } else {
            set(key.c_str(), Va("%.9g", value.get<double>()));  // round-trips a float exactly
        }
    }
    return info;
}

bool DecodeRuleset(std::string_view cs, std::string& error) {
    try {
        const std::string info(cs);
        const auto value = [&info](const char* key) { return std::string(Info_ValueForKey(info.c_str(), key)); };

        int index = -1;
        const std::string indexText = value("index");
        const std::string id = value("id");
        if (std::from_chars(indexText.data(), indexText.data() + indexText.size(), index).ec != std::errc() ||
            index < 0 || index > 255 || id.empty()) {
            throw std::runtime_error("malformed ruleset configstring");
        }

        // back to the JSON shape, typed like a full ToJson()
        json j = json::object();
        const json shape = json::parse(ToJson(Vq3()));
        for (const auto& [key, like] : shape.items()) {
            const std::string v = value(key.c_str());
            if (v.empty()) {
                continue;
            }
            if (like.is_string()) {
                j[key] = v;
            } else if (like.is_boolean()) {
                j[key] = v == "1";
            } else if (like.is_number_integer()) {
                j[key] = std::stoi(v);
            } else {
                // std::stof throws out_of_range on a subnormal, which used to drop the
                // whole configstring: read the value as a double and let Read() judge it,
                // snapping anything below the smallest normal to 0 (what it means in the
                // physics) so a tiny value can never cost the client the whole ruleset
                char* end = nullptr;
                const double d = std::strtod(v.c_str(), &end);
                if (end == v.c_str() || *end != '\0') {
                    throw std::runtime_error("bad number: " + v);
                }
                j[key] = std::fabs(d) < static_cast<double>(std::numeric_limits<float>::min()) ? 0.0 : d;
            }
        }
        SetRulesetAt(index, FromObject(id, j));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace skaar::move
