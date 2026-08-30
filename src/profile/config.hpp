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
// `match = "manual" | "auto"` (optional, default manual). The structured
// form declares what a profile is for (F8):
//
//   [profiles.ipad.match]
//   auto_select = true
//   portrait = true              # or device_name / resolution / aspect_ratio
//   priority = 0                 # tie-break among equally specific matches
//
// Auto selection order: exact device identity > exact resolution > aspect
// ratio > orientation. Profiles without auto_select = true are never picked
// automatically; the default_profile is the final fallback.

#include "domain/error.hpp"
#include "domain/game.hpp"
#include "domain/profile.hpp"
#include "platform/win32/display.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hoyoflux::profile {

enum class LauncherRegion { Auto, Cn, Global };

struct LauncherConfig {
    GameId game{GameId::Genshin};
    std::string profile{"auto"};
    LauncherRegion region{LauncherRegion::Auto};
    bool notifications{true};
};

struct Config {
    std::vector<Profile> profiles;
    std::string default_profile;  // legacy fallback
    std::string genshin_default;
    std::string starrail_default;
    LauncherConfig launcher;
};

struct DisplayFacts {
    win32::DisplayInfo info;
    Resolution resolution{0, 0};
    uint32_t refresh_rate{0};
    float aspect_ratio{0.0f};
    bool portrait{false};
};

struct AutoCandidateDecision {
    std::string profile_id;
    std::optional<uint32_t> display_index;
    int specificity{-1};
    int priority{0};
};

struct AutoProfileDecision {
    Profile profile;
    bool used_fallback{false};
    std::optional<uint32_t> display_index;
    int specificity{0};
    int priority{0};
    std::vector<DisplayFacts> displays;
    std::vector<AutoCandidateDecision> candidates;
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

Result<AutoProfileDecision> resolve_auto_profile(
    const Config& config, GameId game,
    const std::vector<win32::DisplayInfo>& displays);

}  // namespace hoyoflux::profile
