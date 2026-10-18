// What the user can change on the Settings page, kept in
// /data/PS5LM/settings.json. Unknown or missing keys keep their defaults, so
// an old file still loads after a new setting is added.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace ps5lm {

struct Settings {
    bool        auto_load = false;  // load default_model at launch
    std::string default_model;      // a path from the library, or empty
    uint32_t    ctx_cap   = 65536;  // the planner's longest context
    std::string kv_type   = "auto"; // "auto", "f16", "q8_0" or "q4_0"
    bool        sounds    = true;
    bool        tools     = true;   // file tools in the browser chat, inside the scratch folder
    bool        introspect = false;  // POST /v1/introspect: raw logits and activations (off by default)
    uint32_t    scratch_limit_gib = 2;  // warn when the scratch folder grows past this
    std::string download_dir = "/data/PS5LM/models";  // where the market saves models

    bool operator==(const Settings &) const = default;
};

Settings load_settings(const char * path);
bool     save_settings(const char * path, const Settings & s);

// The control port's view (control.cpp): the settings as one JSON object, and
// a partial object applied over them. A key that is unknown, of the wrong
// type or out of range is refused with `error` and changes nothing.
std::string settings_json(const Settings & s);
bool        patch_settings(const std::string & json_text, Settings * s, std::string * error);

}  // namespace ps5lm
