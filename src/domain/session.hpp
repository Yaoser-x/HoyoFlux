#pragma once

// Session identity and lifecycle state.

#include "game.hpp"

#include <cstdint>
#include <string>

namespace hoyoflux {

using SessionId = std::string;

enum class SessionStage {
    Idle,
    Preparing,
    Launching,
    Resolving,
    Patching,
    Running,
    Restoring,
    Completed,
    Failed,
};

struct SessionContext {
    SessionId id;
    GameId game{GameId::Genshin};
    uint32_t pid{0};
    SessionStage stage{SessionStage::Idle};
    bool rollback_required{false};
};

}  // namespace hoyoflux
