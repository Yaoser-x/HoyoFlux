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
#include "platform/win32/unique_handle.hpp"
#include "domain/session.hpp"
#include "game/game_adapter.hpp"

#include <cstdint>

namespace hoyoflux::session {

// One named mutex serializes all recovery and launch work for the current
// Windows user. The kernel releases ownership if the launcher process dies.
class SessionLease {
public:
    static Result<SessionLease> acquire();

    SessionLease() = default;
    ~SessionLease();
    SessionLease(const SessionLease&) = delete;
    SessionLease& operator=(const SessionLease&) = delete;
    SessionLease(SessionLease&& other) noexcept;
    SessionLease& operator=(SessionLease&& other) noexcept;

    [[nodiscard]] bool owns() const noexcept { return owns_; }

private:
    explicit SessionLease(win32::UniqueHandle mutex) noexcept
        : mutex_(std::move(mutex)), owns_(true) {}

    win32::UniqueHandle mutex_;
    bool owns_{false};
};

enum class RecoveryAction {
    None,              // no journal present
    GameStillRunning,  // journal existed, its game process is still alive:
                       // automatic recovery refused
    Recovered,         // stale journal: recorded state restored AND verified,
                       // journal cleared afterwards
    RecoveryFailed,    // restore could not be completed or verified: the
                       // journal is KEPT so recovery can be retried
};

struct SessionConfig {
    game::Region region{game::Region::Auto};
    // How long to wait for engine modules (UnityPlayer.dll, UserAssembly.dll,
    // GameAssembly.dll) to be loaded by the game after it starts running.
    uint32_t module_wait_timeout_ms{60000};
    uint32_t module_poll_interval_ms{100};
    bool verbose{false};
};

class SessionEngine {
public:
    explicit SessionEngine(game::GameAdapter& adapter,
                           SessionConfig config = {});

    // Runs the full non-resident session and blocks until the game exits.
    Result<SessionContext> run(const LaunchRequest& request);
    Result<SessionContext> run(const LaunchRequest& request,
                               SessionLease& lease);
    // The caller must hold the lease and have completed preflight. This is
    // used by LaunchService so One-click can notify only after preflight PASS.
    Result<SessionContext> run_after_preflight(const LaunchRequest& request,
                                               SessionLease& lease);

    // Called on every launcher start: if a journal from a previous crash
    // exists and its game process is gone, performs the recorded rollback
    // (game persistent state, physical displays), VERIFIES the restore, and
    // only then clears the journal (plan §10.3). A live game process is
    // never touched.
    Result<RecoveryAction> recover();
    Result<RecoveryAction> recover(SessionLease& lease);
    Result<void> preflight(SessionLease& lease);

private:
    game::GameAdapter& adapter_;
    SessionConfig config_;
};

}  // namespace hoyoflux::session
