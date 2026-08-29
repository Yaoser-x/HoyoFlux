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

    // Absent = do not pass -screen-fullscreen at all; the game keeps its own
    // current mode. Only modes a game adapter can express as verified launch
    // arguments may be set (borderless is not one of them - see the
    // adapters' capability reports).
    std::optional<FullscreenMode> fullscreen;

    ResolutionPersistence persistence{ResolutionPersistence::Session};
    std::optional<uint32_t> monitor;  // display index; 0 = primary
};

struct RuntimePolicy {
    uint32_t fps{120};
    PowerSavePolicy power_save{PowerSavePolicy::Disabled};
    uint32_t power_save_fps{30};
    ProcessPriority priority{ProcessPriority::Normal};
    // F7: END toggles the fps control on/off, Ctrl+Up/Down step the fps.
    // Off = no hotkey thread exists at all.
    bool hotkeys{false};
};

struct UiPolicy {
    bool mobile_ui{false};
    std::optional<float> dpi_scale;  // 1.0 == 100%; absent == leave game default
};

// How a profile is selected when the user asks for `--profile auto` (F8).
// A profile declares WHAT it is for; the matcher walks the displays and
// picks by identity before geometry. A profile with auto_select=false is
// never chosen automatically - only an explicit --profile can select it.
struct MatchPolicy {
    bool auto_select{false};

    std::optional<std::wstring> device_name;   // exact monitor identity
    std::optional<Resolution> resolution;      // exact current resolution
    std::optional<float> aspect_ratio;         // e.g. 0.5625 (9:16 portrait)
    std::optional<bool> portrait;              // orientation
    int priority{0};  // higher wins among equally specific candidates
};

using ProfileId = std::string;

struct Profile {
    ProfileId id;
    GameId game{GameId::Genshin};
    RenderPolicy render;
    UiPolicy ui;
    RuntimePolicy runtime;
    MatchPolicy match{};  // auto_select=false: manual by default
};

}  // namespace hoyoflux
