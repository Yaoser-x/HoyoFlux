#pragma once

// Game identity and installation facts.

#include <filesystem>
#include <string_view>

namespace hoyoflux {

enum class GameId { Genshin, StarRail };

constexpr std::string_view to_string(GameId id) {
    switch (id) {
    case GameId::Genshin: return "genshin";
    case GameId::StarRail: return "starrail";
    }
    return "unknown";
}

// A located game installation.
struct GameInstall {
    GameId game{GameId::Genshin};
    bool is_cn{true};
    std::filesystem::path exe_path;  // absolute path to the game executable
};

}  // namespace hoyoflux
