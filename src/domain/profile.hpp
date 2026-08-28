#pragma once

// Profile model: what one game session should look like.
//
// A Profile fully describes a launch: render policy, UI policy, runtime
// policy and how it is selected. Profiles are stored in TOML (see the
// profile module) and parsed once at startup into typed structs.

#include "game.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hoyoflux {

struct Resolution {
    uint32_t width{0};
    uint32_t height{0};

    [[nodiscard]] bool empty() const { return width == 0 || height == 0; }
    friend bool operator==(const Resolution&, const Resolution&) = default;
};

enum class FullscreenMode { Exclusive, Borderless, Windowed };

// Where the requested resolution lives after the game session ends.
//
//   Persistent - written into the game's persistent settings (pollutes the
//                launcher's config; this is what the legacy tool effectively
//                did and what we deliberately avoid by default).
//   Session    - applied only for this session and restored afterwards.
//
// Session is the default and the safety-critical choice.
enum class ResolutionPersistence { Persistent, Session };

enum class ProcessPriority { Realtime, High, AboveNormal, Normal, BelowNormal };

// FPS the game should drop to when the power-saving policy is active.
enum class PowerSavePolicy { Disabled, Enabled };

struct RenderPolicy {
    std::optional<Resolution> resolution;
    FullscreenMode fullscreen{FullscreenMode::Borderless};
    ResolutionPersistence persistence{ResolutionPersistence::Session};
    std::optional<uint32_t> monitor;  // display index; 0 = primary
};

struct RuntimePolicy {
    uint32_t fps{120};
    PowerSavePolicy power_save{PowerSavePolicy::Disabled};
    uint32_t power_save_fps{30};
    ProcessPriority priority{ProcessPriority::Normal};
};

struct UiPolicy {
    bool mobile_ui{false};
    std::optional<float> dpi_scale;  // 1.0 == 100%; absent == leave game default
};

// How a profile is selected when the user asks for `--profile auto`.
enum class MatchPolicy { Manual, Auto };

using ProfileId = std::string;

struct Profile {
    ProfileId id;
    GameId game{GameId::Genshin};
    RenderPolicy render;
    UiPolicy ui;
    RuntimePolicy runtime;
    MatchPolicy match{MatchPolicy::Manual};
};

}  // namespace hoyoflux
