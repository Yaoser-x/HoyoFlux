#pragma once

// TOML profile store (A9): %LOCALAPPDATA%\HoyoFlux\config.toml, parsed once
// at startup into typed domain::Profile values. Never touched on the hot
// path - the session engine only receives resolved structs.
//
// Schema (all fields optional; missing values keep the defaults below):
//
//   default_profile = "desktop"
//
//   [profiles.desktop.render]
//   resolution = "2560x1440"     # "WxH"; absent = leave as-is
//   fullscreen = "windowed"     # exclusive | windowed; absent = leave as-is
//                               # (borderless parses but no game can set it
//                               # via launch arguments, so it fails the gate)
//   persistence = "session"     # session | persistent
//   monitor = 0                 # display index; absent = primary
//
//   [profiles.desktop.runtime]
//   fps = 120
//   priority = "normal"          # realtime | high | above_normal | normal
//                                # | below_normal
//
//   [profiles.desktop.runtime.power_save]
//   enabled = false
//   fps = 30
//
//   [profiles.desktop.ui]
//   mobile_ui = false
//   dpi_scale = 1.0             # absent = leave game default
//
// Every profile also takes `game = "genshin" | "starrail"` (required) and
// `match = "manual" | "auto"` (optional, default manual).

#include "domain/error.hpp"
#include "domain/game.hpp"
#include "domain/profile.hpp"
#include "platform/win32/display.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hoyoflux::profile {

struct Config {
    std::vector<Profile> profiles;
    std::string default_profile;  // meaningful when profiles are present
};

// The document written when no config file exists (also the documentation).
[[nodiscard]] std::string default_config_toml();

// Parse a config document. Unknown keys are ignored (forward compatibility);
// malformed TOML or bad field values are errors naming the offending field.
Result<Config> parse_config(std::string_view toml_text);

// Load %LOCALAPPDATA%\HoyoFlux\config.toml. A missing file yields the
// default document (desktop/ipad/xiaomi presets); a broken one is an error
// the user must fix (`hoyoflux doctor` shows it).
Result<Config> load_config(const std::filesystem::path& path);

// Find a profile by id.
Result<Profile> find_profile(const Config& config, std::string_view id);

// `--profile auto`: pick the profile for `game` from the attached displays.
// Heuristic: with a portrait display attached, the first mobile-UI profile
// for the game; otherwise the first non-mobile profile for the game.
Result<Profile> match_auto_profile(const Config& config, GameId game,
                                   const std::vector<win32::DisplayInfo>& displays);

}  // namespace hoyoflux::profile
