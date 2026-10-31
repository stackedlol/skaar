// This file is part of Skaar. GPLv2 or later.
#include "arenalog.h"

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

// One JSON value as T. Numbers are checked while they are still wide: get<T>()
// narrows with no range test, which is UB out of range and truncates a fractional
// int. The history file is a plain file the player may hand-edit.
template <typename T>
T Read(const json& v, const char* what, T fallback) {
    if (v.is_null()) {
        return fallback;
    }
    if constexpr (std::is_floating_point_v<T>) {
        const double d = v.get<double>();
        if (!std::isfinite(d)) {
            throw std::runtime_error(std::string(what) + " is out of range");
        }
        const double hi = static_cast<double>(std::numeric_limits<T>::max());
        return static_cast<T>(std::clamp(d, -hi, hi));
    } else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        if (!v.is_number_integer()) {
            throw std::runtime_error(std::string(what) + " must be a whole number");
        }
        const int64_t n = v.get<int64_t>();
        if (n < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
            n > static_cast<int64_t>(std::numeric_limits<T>::max())) {
            throw std::runtime_error(std::string(what) + " is out of range");
        }
        return static_cast<T>(n);
    } else {
        return v.get<T>();
    }
}

template <typename T>
T Get(const json& j, const char* key, T fallback) {
    auto it = j.find(key);
    return it == j.end() ? fallback : Read<T>(*it, key, fallback);
}

}  // namespace

std::string ArenaSessionToJsonLine(const ArenaSession& s) {
    json weapons = json::object();
    for (const auto& [id, w] : s.weapons) {
        weapons[id] = {w.shots, w.hits};
    }
    json j = {{"when", s.when.Iso()},
              {"map", s.map},
              {"ruleset", s.ruleset},
              {"seconds", std::round(static_cast<double>(s.seconds) * 10) / 10},
              {"damageDealt", s.damageDealt},
              {"damageTaken", s.damageTaken},
              {"kills", s.kills},
              {"deaths", s.deaths},
              {"weapons", weapons}};
    // a map name or g_movement value is bytes from a config file or a map list, so
    // they need not be UTF-8: replace them rather than throw into the server's path
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool ArenaSessionFromJson(std::string_view line, ArenaSession& out) {
    const json j = json::parse(line.begin(), line.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("when")) {
        return false;
    }
    try {
        ArenaSession s;
        if (!RunTime::FromIso(j.at("when").get<std::string>(), s.when)) {
            return false;
        }
        s.map = Get<std::string>(j, "map", "");
        s.ruleset = Get<std::string>(j, "ruleset", "vq3");
        // the same cast-and-format path as RunSummary::duration: keep it to one day
        s.seconds = std::clamp(Get<float>(j, "seconds", 0), 0.0f, 86400.0f);
        s.damageDealt = Get<int>(j, "damageDealt", 0);
        s.damageTaken = Get<int>(j, "damageTaken", 0);
        s.kills = Get<int>(j, "kills", 0);
        s.deaths = Get<int>(j, "deaths", 0);
        if (j.contains("weapons")) {
            for (const auto& [id, v] : j.at("weapons").items()) {
                s.weapons[id] = {Read<int>(v.at(0), "weapons", 0), Read<int>(v.at(1), "weapons", 0)};
            }
        }
        out = std::move(s);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<ArenaSession> ParseArenaHistory(std::string_view text) {
    std::vector<ArenaSession> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        ArenaSession s;
        if (ArenaSessionFromJson(text.substr(start, end - start), s)) {
            out.push_back(std::move(s));
        }
        start = end + 1;
    }
    return out;
}

}  // namespace skaar
