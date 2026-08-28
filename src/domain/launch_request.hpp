#pragma once

// A fully resolved request to launch one game session.
//
// Produced by the application layer from a Profile plus CLI overrides,
// validated, and handed to the SessionEngine. After construction the engine
// treats it as immutable.

#include "game.hpp"
#include "profile.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hoyoflux {

struct LaunchRequest {
    GameId game{GameId::Genshin};
    Profile profile;

    // Raw command-line tokens forwarded verbatim to the game process.
    std::vector<std::wstring> game_args;

    // true when a runtime controller (display guard, hotkeys, dynamic FPS)
    // must stay attached to the session. Fixed profiles run non-resident.
    bool resident{false};

    // Overrides for doctor / explicit path use.
    std::optional<std::filesystem::path> exe_override;
};

}  // namespace hoyoflux
