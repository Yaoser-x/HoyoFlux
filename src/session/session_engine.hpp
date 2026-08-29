#pragma once

// SessionEngine: owns the whole session lifecycle. It is the ONLY module
// allowed to orchestrate: adapters answer questions, the patch engine writes
// memory, the journal remembers - none of them terminate processes, restore
// displays or clear state on their own.
//
// Lifecycle (fixed profile, non-resident):
//   Preparing -> locate install, snapshot displays, write journal
//   Launching -> CreateProcess(SUSPENDED)
//   Resolving -> wait for required modules, snapshot sections, resolve
//                signatures
//   Patching  -> build + apply the patch plan (RemoteState included)
//   Running   -> resume the game, wait for exit
//   Restoring -> display restore hook (A8), clear journal
//   Completed
// Any failure before the game runs: roll back patches, terminate the game,
// journal -> Failed -> cleared. A launcher crash instead leaves the journal
// behind; the next launch's recover() cleans it up.

#include "domain/error.hpp"
#include "domain/launch_request.hpp"
#include "domain/session.hpp"
#include "game/game_adapter.hpp"

#include <cstdint>

namespace hoyoflux::session {

enum class RecoveryAction {
    None,                 // no journal present
    CleanedStaleJournal,  // journal existed, its game process is dead
    GameStillRunning,     // journal existed, its game process is still alive
};

struct SessionConfig {
    game::Region region{game::Region::Auto};
    // How long to wait for engine modules (UnityPlayer.dll, UserAssembly.dll,
    // GameAssembly.dll) to be loaded by the game after it starts running.
    uint32_t module_wait_timeout_ms{60000};
    uint32_t module_poll_interval_ms{100};
};

class SessionEngine {
public:
    explicit SessionEngine(game::GameAdapter& adapter,
                           SessionConfig config = {});

    // Runs the full non-resident session and blocks until the game exits.
    Result<SessionContext> run(const LaunchRequest& request);

    // Called on every launcher start: clean up a leftover journal from a
    // previous crash. Never touches a still-running game.
    Result<RecoveryAction> recover();

private:
    game::GameAdapter& adapter_;
    SessionConfig config_;
};

}  // namespace hoyoflux::session
