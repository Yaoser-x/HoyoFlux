#pragma once

// GameLaunchPlan (F1): everything needed to start one game process.
//
// Built exclusively by GameAdapters - the session engine and CLI never
// compose game arguments themselves. The adapter owns the mapping from
// Profile (render policy) to the game's actual launch mechanism and
// validates user passthrough against the fields it manages.

#include "domain/error.hpp"
#include "domain/profile.hpp"

#include <filesystem>
#include <vector>

namespace hoyoflux::game {

struct GameLaunchPlan {
    std::filesystem::path executable;
    std::filesystem::path working_directory;

    // Full argv for CreateProcessW, argv[0] (the executable) included. Each
    // token is ONE logical argument - quoting happens once, in the win32
    // process layer (quote_windows_argument).
    std::vector<std::wstring> arguments;

    ProcessPriority priority{ProcessPriority::Normal};
};

// Render policy -> Unity launch arguments (shared by both adapters, which
// are Unity games). Only mechanisms the F1 plan sanctions:
//   resolution set          -> -screen-width W -screen-height H
//   fullscreen == Windowed  -> -screen-fullscreen 0
//   fullscreen == Exclusive -> -screen-fullscreen 1
// Borderless is deliberately NOT mapped: it is not expressible as a Unity
// launch argument, so requesting it is a capability error (F0), not a guess.
[[nodiscard]] Result<std::vector<std::wstring>> build_render_arguments(
    const RenderPolicy& render);

// Names of the launch fields HoyoFlux manages from the render policy. User
// passthrough that defines any of them would create duplicate definitions,
// so merge_passthrough rejects it instead of guessing precedence.
[[nodiscard]] bool is_managed_launch_field(std::wstring_view token);

// Append verbatim user arguments after the managed ones, rejecting tokens
// that define a managed field. `context` names the game in error messages.
[[nodiscard]] Result<std::vector<std::wstring>> merge_passthrough(
    const std::vector<std::wstring>& managed,
    const std::vector<std::wstring>& passthrough, std::string_view context);

}  // namespace hoyoflux::game
