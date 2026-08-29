#include "session/session_engine.hpp"

#include "patch/patch_engine.hpp"
#include "platform/win32/display.hpp"
#include "platform/win32/process.hpp"
#include "scan/module_snapshot.hpp"
#include "session/display_guard.hpp"
#include "session/journal.hpp"

#include <windows.h>

#include <chrono>
#include <string>
#include <thread>

namespace hoyoflux::session {
namespace {

using game::ModuleRequirements;
using game::PatchContext;

DWORD priority_class(ProcessPriority priority) {
    switch (priority) {
    case ProcessPriority::Realtime: return REALTIME_PRIORITY_CLASS;
    case ProcessPriority::High: return HIGH_PRIORITY_CLASS;
    case ProcessPriority::AboveNormal: return ABOVE_NORMAL_PRIORITY_CLASS;
    case ProcessPriority::BelowNormal: return BELOW_NORMAL_PRIORITY_CLASS;
    case ProcessPriority::Normal: break;
    }
    return NORMAL_PRIORITY_CLASS;
}

SessionId make_session_id() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "session-" + std::to_string(now) + "-" +
           std::to_string(GetCurrentProcessId());
}

// Modules the flow must read: the main module (empty name) plus any named
// module the adapter asked for, in requirement order.
Result<std::vector<uintptr_t>> locate_modules(
    const win32::UniqueHandle& process, const ModuleRequirements& requirements) {
    std::vector<uintptr_t> bases;
    bases.reserve(requirements.modules.size());
    for (const auto& requirement : requirements.modules) {
        auto base = requirement.module.empty()
                        ? scan::remote_module_base(process)
                        : scan::remote_module_base(process, requirement.module);
        if (!base) {
            return std::unexpected(base.error());
        }
        bases.push_back(*base);
    }
    return bases;
}

}  // namespace

SessionEngine::SessionEngine(game::GameAdapter& adapter, SessionConfig config)
    : adapter_(adapter), config_(config) {}

Result<RecoveryAction> SessionEngine::recover() {
    auto journal = load_journal();
    if (!journal) {
        return std::unexpected(journal.error());
    }
    if (!journal->has_value()) {
        return RecoveryAction::None;
    }
    const ActiveSessionJournal& stale = **journal;
    if (stale.pid != 0 && win32::is_process_running(stale.pid)) {
        // Something under that pid is alive: never touch a running game.
        // `hoyoflux recover` reports this so the user can decide.
        return RecoveryAction::GameStillRunning;
    }
    if (auto cleared = clear_journal(); !cleared) {
        return std::unexpected(cleared.error());
    }
    return RecoveryAction::CleanedStaleJournal;
}

Result<SessionContext> SessionEngine::run(const LaunchRequest& request) {
    SessionContext context;
    context.id = make_session_id();
    context.game = request.game;

    // ---- Preparing: locate install, snapshot displays, write journal -----
    context.stage = SessionStage::Preparing;
    auto install = adapter_.locate_installation(config_.region);
    if (!install) {
        return std::unexpected(install.error());
    }
    if (request.exe_override) {
        install->exe_path = *request.exe_override;
    }
    auto old_version = adapter_.is_old_version(*install);
    if (!old_version) {
        return std::unexpected(old_version.error());
    }
    auto requirements = adapter_.module_requirements(*install, request.profile);
    if (!requirements) {
        return std::unexpected(requirements.error());
    }
    // F0 gate: a profile feature the adapter cannot honor stops the launch
    // here - before the game process, journal or patches exist.
    if (auto valid = game::validate_profile(request.profile,
                                            adapter_.capabilities(*install,
                                                                  request.profile));
        !valid) {
        return std::unexpected(valid.error());
    }

    ActiveSessionJournal journal;
    journal.session_id = context.id;
    journal.game = request.game;
    journal.stage = SessionStage::Preparing;
    if (auto displays = win32::enumerate_displays(); displays) {
        for (const auto& display : *displays) {
            if (!display.is_attached) {
                continue;
            }
            if (auto settings =
                    win32::query_current_settings(display.device_name);
                settings) {
                journal.displays.push_back(JournalDisplay{*settings});
            }
        }
    }
    if (auto saved = save_journal(journal); !saved) {
        return std::unexpected(saved.error());
    }

    // Everything from here on shares the same failure handling: stop the
    // still-suspended game we own, undo uncommitted patches, journal ->
    // Failed -> cleared. Safe because none of this runs after the game has
    // executed patched code (patches die with the process anyway).
    struct SessionFailure {
        const win32::LaunchedProcess* launched{nullptr};
        patch::AppliedPatch* applied{nullptr};
        bool patches_live{false};
        Error error;
    };
    auto finish_failed = [&](SessionFailure failure) -> Result<SessionContext> {
        if (failure.launched != nullptr) {
            if (failure.applied != nullptr && !failure.patches_live) {
                patch::rollback_patch_plan(failure.launched->process, *failure.applied);
            }
            if (!failure.patches_live ||
                win32::is_process_running(failure.launched->pid)) {
                win32::terminate_and_wait(failure.launched->process, 5000);
            }
        }
        journal.stage = SessionStage::Failed;
        save_journal(journal);
        clear_journal();
        context.stage = SessionStage::Failed;
        return std::unexpected(std::move(failure.error));
    };

    // ---- Launching --------------------------------------------------------
    // The adapter owns the game-argument mapping (plan F1); the engine only
    // executes the resulting plan and never sees a Unity flag.
    context.stage = SessionStage::Launching;
    auto launch_plan = adapter_.build_launch_plan(*install, request);
    if (!launch_plan) {
        journal.stage = SessionStage::Failed;
        save_journal(journal);
        clear_journal();
        return std::unexpected(launch_plan.error());
    }
    auto launched =
        win32::spawn_suspended(launch_plan->executable, launch_plan->arguments,
                               launch_plan->working_directory,
                               priority_class(launch_plan->priority));
    if (!launched) {
        journal.stage = SessionStage::Failed;
        save_journal(journal);
        clear_journal();
        return std::unexpected(launched.error());
    }
    context.pid = launched->pid;
    journal.pid = launched->pid;
    journal.stage = SessionStage::Resolving;
    save_journal(journal);

    // ---- Resolving --------------------------------------------------------
    // Engine modules (UnityPlayer.dll etc.) only exist once the game's
    // loader has run. Main-module-only sessions snapshot right away; the
    // others resume, wait for the modules to appear, then re-suspend for
    // patching - external only, no remote thread, no injection.
    context.stage = SessionStage::Resolving;
    bool game_has_run = false;
    std::vector<scan::ModuleSnapshot> snapshots;
    std::vector<uintptr_t> bases;
    auto located = locate_modules(launched->process, *requirements);
    if (!located) {
        // Give the loader a chance, then re-suspend for a stable snapshot.
        if (auto resumed = win32::resume_process_threads(launched->pid); !resumed) {
            return finish_failed({&*launched, nullptr, false, resumed.error()});
        }
        game_has_run = true;
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(config_.module_wait_timeout_ms);
        while (!(located = locate_modules(launched->process, *requirements))) {
            if (!win32::is_process_running(launched->pid) ||
                std::chrono::steady_clock::now() > deadline) {
                return finish_failed(
                    {&*launched, nullptr, false,
                     Error::make(ErrorCode::ModuleNotFound,
                                 "game did not load its required modules in time")});
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.module_poll_interval_ms));
        }
        if (auto suspended = win32::suspend_process_threads(launched->pid);
            !suspended) {
            return finish_failed({&*launched, nullptr, false, suspended.error()});
        }
    }
    bases = *located;

    for (size_t i = 0; i < requirements->modules.size(); ++i) {
        const auto& requirement = requirements->modules[i];
        std::vector<std::string_view> sections(requirement.sections.begin(),
                                               requirement.sections.end());
        auto snapshot = scan::snapshot_module(launched->process, bases[i], sections);
        if (!snapshot) {
            return finish_failed({&*launched, nullptr, game_has_run, snapshot.error()});
        }
        snapshots.push_back(std::move(*snapshot));
    }

    auto resolved = adapter_.resolve_signatures(snapshots);
    if (!resolved) {
        return finish_failed({&*launched, nullptr, game_has_run, resolved.error()});
    }

    // ---- Patching ---------------------------------------------------------
    context.stage = SessionStage::Patching;
    journal.stage = SessionStage::Patching;
    journal.rollback_required = true;
    save_journal(journal);

    PatchContext patch_context{*resolved, request.profile,
                               bases.empty() ? 0 : bases.front(), *old_version};
    auto plan = adapter_.build_patch_plan(patch_context);
    if (!plan) {
        return finish_failed({&*launched, nullptr, game_has_run, plan.error()});
    }
    patch::AppliedPatch applied;
    auto apply_result = patch::apply_patch_plan(launched->process, *plan);
    if (!apply_result) {
        return finish_failed({&*launched, nullptr, game_has_run, apply_result.error()});
    }
    applied = std::move(*apply_result);

    // ---- Running ----------------------------------------------------------
    // Past this point patched pages may execute: external rollback is no
    // longer safe, and everything in the process - patches included - dies
    // with it.
    context.stage = SessionStage::Running;
    journal.stage = SessionStage::Running;
    journal.rollback_required = false;
    save_journal(journal);

    if (game_has_run) {
        if (auto resumed = win32::resume_process_threads(launched->pid); !resumed) {
            return finish_failed({&*launched, &applied, false, resumed.error()});
        }
    } else {
        ResumeThread(launched->thread.get());
    }

    WaitForSingleObject(launched->process.get(), INFINITE);

    // ---- Restoring / Completed -------------------------------------------
    context.stage = SessionStage::Restoring;
    // Plan-risk-3 fallback: until the game's resolution storage is identified
    // on a live machine, unconditionally restore the captured snapshot.
    if (auto restored = restore_display_snapshot(journal.displays); !restored) {
        context.stage = SessionStage::Failed;
        clear_journal();
        return std::unexpected(restored.error());
    }
    if (auto cleared = clear_journal(); !cleared) {
        context.stage = SessionStage::Failed;
        return std::unexpected(cleared.error());
    }
    context.stage = SessionStage::Completed;
    return context;
}

}  // namespace hoyoflux::session
