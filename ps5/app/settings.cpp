// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace ps5lm {

namespace {

// Every key over its default; a key of the wrong type throws.
void apply(const nlohmann::json & j, Settings & s) {
    s.auto_load     = j.value("auto_load", s.auto_load);
    s.default_model = j.value("default_model", s.default_model);
    s.ctx_cap       = j.value("ctx_cap", s.ctx_cap);
    s.kv_type       = j.value("kv_type", s.kv_type);
    s.sounds        = j.value("sounds", s.sounds);
    s.tools         = j.value("tools", s.tools);
    s.introspect    = j.value("introspect", s.introspect);
    s.scratch_limit_gib = j.value("scratch_limit_gib", s.scratch_limit_gib);
    s.download_dir  = j.value("download_dir", s.download_dir);
}

nlohmann::json to_json(const Settings & s) {
    return {
        { "auto_load", s.auto_load }, { "default_model", s.default_model }, { "ctx_cap", s.ctx_cap },
        { "kv_type", s.kv_type },     { "sounds", s.sounds },       { "tools", s.tools },
        { "introspect", s.introspect },
        { "scratch_limit_gib", s.scratch_limit_gib }, { "download_dir", s.download_dir },
    };
}

}  // namespace

Settings load_settings(const char * path) {
    Settings      s;
    std::ifstream in(path);
    if (!in) {
        return s;
    }
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) {
        return s;
    }
    apply(j, s);
    return s;
}

bool save_settings(const char * path, const Settings & s) {
    std::ofstream out(path);
    out << to_json(s).dump(2) << "\n";
    return (bool) out;
}

std::string settings_json(const Settings & s) { return to_json(s).dump(); }

bool patch_settings(const std::string & json_text, Settings * s, std::string * error) {
    const nlohmann::json j = nlohmann::json::parse(json_text, nullptr, false);
    if (!j.is_object()) {
        *error = "the body is not a JSON object";
        return false;
    }
    const nlohmann::json known = to_json(*s);
    for (const auto & [key, v] : j.items()) {
        if (!known.contains(key)) {
            *error = "unknown setting \"" + key + "\"";
            return false;
        }
        const auto & was = known[key];
        const bool same = was.is_number() ? v.is_number_integer() && v.get<int64_t>() >= 0 && v.get<int64_t>() <= 0xffffffffLL
                                          : v.type() == was.type();
        if (!same) {
            *error = "\"" + key + "\" takes " + (was.is_number() ? std::string("a whole number >= 0") : std::string(was.type_name()));
            return false;
        }
    }
    Settings out = *s;
    apply(j, out);
    if (out.kv_type != "auto" && out.kv_type != "f16" && out.kv_type != "q8_0" && out.kv_type != "q4_0") {
        *error = "kv_type is one of auto, f16, q8_0, q4_0";
        return false;
    }
    if (out.ctx_cap < 512) {
        *error = "ctx_cap is at least 512";
        return false;
    }
    *s = out;
    return true;
}

}  // namespace ps5lm
