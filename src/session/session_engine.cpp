#include "session/session_engine.hpp"

#include "patch/patch_engine.hpp"
#include "platform/win32/process.hpp"
#include "scan/module_snapshot.hpp"
#include "session/display_guard.hpp"
#include "session/journal.hpp"
#include "patch/memory_writer.hpp"
#include "patch/remote_state.hpp"
#include "runtime/runtime_controller.hpp"
#include "session/persistent_state_guard.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

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

std::string hex_address(uintptr_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value;
    return out.str();
}

std::string hex_bytes(const std::vector<std::byte>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            out << ' ';
        }
        out << std::setw(2) << std::to_integer<unsigned>(bytes[i]);
    }
    return out.str();
}

}  // namespace

SessionEngine::SessionEngine(game::GameAdapter& adapter, SessionConfig config)
    : adapter_(adapter), config_(config) {}

SessionLease::~SessionLease() {
    if (owns_ && mutex_) {
        ReleaseMutex(mutex_.get());
    }
}

SessionLease::SessionLease(SessionLease&& other) noexcept
    : mutex_(std::move(other.mutex_)), owns_(std::exchange(other.owns_, false)) {}

SessionLease& SessionLease::operator=(SessionLease&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (owns_ && mutex_) {
        ReleaseMutex(mutex_.get());
    }
    mutex_ = std::move(other.mutex_);
    owns_ = std::exchange(other.owns_, false);
    return *this;
}

Result<SessionLease> SessionLease::acquire() {
    HANDLE raw = CreateMutexW(nullptr, FALSE, L"Local\\HoyoFlux-SessionLease");
    if (raw == nullptr) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "CreateMutexW for session lease failed",
            GetLastError()));
    }
    win32::UniqueHandle mutex(raw);
    const DWORD wait = WaitForSingleObject(mutex.get(), 0);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        return SessionLease(std::move(mutex));
    }
    if (wait == WAIT_TIMEOUT) {
        return std::unexpected(Error::make(
            ErrorCode::SessionAlreadyActive,
            "session-already-active: another HoyoFlux session or recovery is "
            "already in progress"));
    }
    return std::unexpected(Error::make(
        ErrorCode::OsError, "WaitForSingleObject for session lease failed",
        GetLastError()));
}

Result<RecoveryAction> SessionEngine::recover() {
    auto lease = SessionLease::acquire();
    if (!lease) {
        return std::unexpected(lease.error());
    }
    return recover(*lease);
}

Result<RecoveryAction> SessionEngine::recover(SessionLease& lease) {
    if (!lease.owns()) {
        return std::unexpected(
            Error::make(ErrorCode::InvalidArgument, "recover requires a session lease"));
    }
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

    // Plan §10.2 order: game persistent state first, then physical displays.
    Error failure{ErrorCode::None, ""};
    bool failed = false;

    if (stale.rollback.persistent_state.has_value()) {
        if (auto restored =
                game::restore_persistent_roots(*stale.rollback.persistent_state);
            !restored) {
            failed = true;
            failure = restored.error();
        }
    }
    if (!failed) {
        if (auto restored = restore_display_snapshot(stale.rollback.displays);
            !restored) {
            failed = true;
            failure = restored.error();
        }
    }
    if (failed) {
        // Plan §10.3: a failed restore NEVER clears the journal; recovery
        // can be retried (or diagnosed) on the next run.
        return RecoveryAction::RecoveryFailed;
    }

    // Verify the persistent-state restore before the journal is cleared.
    if (stale.rollback.persistent_state.has_value()) {
        std::vector<std::wstring> roots;
        for (const auto& set : stale.rollback.persistent_state->sets) {
            roots.push_back(set.root);
        }
        auto verify = game::snapshot_persistent_roots(roots);
        if (!verify || !game::persistent_state_equals(
                           *stale.rollback.persistent_state, *verify)) {
            return RecoveryAction::RecoveryFailed;
        }
    }

    if (auto cleared = clear_journal(); !cleared) {
        return std::unexpected(cleared.error());
    }
    return RecoveryAction::Recovered;
}

Result<void> SessionEngine::preflight(SessionLease& lease) {
    if (!lease.owns()) {
        return std::unexpected(Error::make(
            ErrorCode::InvalidArgument, "session preflight requires a session lease"));
    }
    auto action = recover(lease);
    if (!action) {
        return std::unexpected(action.error());
    }
    if (*action == RecoveryAction::GameStillRunning) {
        return std::unexpected(Error::make(
            ErrorCode::SessionAlreadyActive,
            "session-already-active: the journal references a live process"));
    }
    if (*action == RecoveryAction::RecoveryFailed) {
        return std::unexpected(Error::make(
            ErrorCode::SessionFailed,
            "automatic recovery failed; recovery journal retained"));
    }
    return {};
}

Result<SessionContext> SessionEngine::run(const LaunchRequest& request) {
    auto lease = SessionLease::acquire();
    if (!lease) {
        return std::unexpected(lease.error());
    }
    return run(request, *lease);
}

Result<SessionContext> SessionEngine::run(const LaunchRequest& request,
                                          SessionLease& lease) {
    auto checked = preflight(lease);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    return run_after_preflight(request, lease);
}

Result<SessionContext> SessionEngine::run_after_preflight(
    const LaunchRequest& request, SessionLease& lease) {
    if (!lease.owns()) {
        return std::unexpected(Error::make(
            ErrorCode::InvalidArgument,
            "run_after_preflight requires a session lease"));
    }
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

    // F2/F3: when the profile drives the render session-scoped, protect the
    // game's own persisted settings: snapshot now, watch while the game
    // runs, restore after it exits. A crash after this point leaves the
    // snapshot in the journal (F4) for recovery.
    const bool protect_persistent_state =
        request.profile.render.resolution.has_value() &&
        request.profile.render.persistence == ResolutionPersistence::Session;
    std::optional<PersistentDisplayState> persistent_snapshot;
    if (protect_persistent_state) {
        auto snap = adapter_.snapshot_persistent_display_state();
        if (!snap) {
            return std::unexpected(snap.error());
        }
        persistent_snapshot = std::move(*snap);
    }

    ActiveSessionJournal journal;
    journal.session_id = context.id;
    journal.game = request.game;
    journal.stage = SessionStage::Preparing;
    if (persistent_snapshot.has_value()) {
        journal.rollback.persistent_state = persistent_snapshot;
    }
    // v1 never changes the Windows physical display mode. Rollback records
    // only state HoyoFlux actually modified, so this remains empty until a
    // future ApplyDisplayMode path records its own successful changes.
    if (auto saved = save_journal(journal); !saved) {
        return std::unexpected(saved.error());
    }

    auto restore_session_state = [&]() -> Result<void> {
        if (persistent_snapshot.has_value()) {
            if (auto restored = adapter_.restore_persistent_display_state(
                    *persistent_snapshot);
                !restored) {
                return std::unexpected(restored.error());
            }
            std::vector<std::wstring> roots;
            roots.reserve(persistent_snapshot->sets.size());
            for (const auto& set : persistent_snapshot->sets) {
                roots.push_back(set.root);
            }
            auto verify = game::snapshot_persistent_roots(roots);
            if (!verify || !game::persistent_state_equals(*persistent_snapshot,
                                                           *verify)) {
                return std::unexpected(Error::make(
                    ErrorCode::SessionFailed,
                    "persistent-state restore verification failed; recovery "
                    "journal retained"));
            }
        }
        if (auto restored = restore_display_snapshot(journal.rollback.displays);
            !restored) {
            return std::unexpected(restored.error());
        }
        return {};
    };

    // Everything from here on shares the same failure handling: stop the
    // game we own, undo uncommitted patches, and restore session state when
    // the game has executed at all. A failed restore retains the journal.
    bool game_has_run = false;
    struct SessionFailure {
        const win32::LaunchedProcess* launched{nullptr};
        patch::AppliedPatch* applied{nullptr};
        bool patches_live{false};
        Error error;
    };
    auto finish_failed = [&](SessionFailure failure) -> Result<SessionContext> {
        bool game_dead = failure.launched == nullptr;
        bool cleanup_failed = false;
        Error cleanup_error;
        const auto record_cleanup_error = [&](const Error& error) {
            if (!cleanup_failed) {
                cleanup_error = error;
            }
            cleanup_failed = true;
        };

        if (failure.launched != nullptr) {
            const DWORD initial_wait =
                WaitForSingleObject(failure.launched->process.get(), 0);
            if (initial_wait == WAIT_OBJECT_0) {
                game_dead = true;
            } else if (initial_wait == WAIT_TIMEOUT) {
                if (failure.applied != nullptr && !failure.patches_live) {
                    if (auto rolled_back = patch::rollback_patch_plan(
                            failure.launched->process, *failure.applied);
                        !rolled_back) {
                        record_cleanup_error(rolled_back.error());
                    }
                }
                if (auto terminated = win32::terminate_and_wait(
                        failure.launched->process, 5000);
                    !terminated) {
                    record_cleanup_error(terminated.error());
                }
                const DWORD final_wait =
                    WaitForSingleObject(failure.launched->process.get(), 0);
                if (final_wait == WAIT_OBJECT_0) {
                    game_dead = true;
                } else if (final_wait == WAIT_FAILED) {
                    record_cleanup_error(Error::make(
                        ErrorCode::OsError,
                        "cannot confirm owned game termination",
                        GetLastError()));
                }
            } else {
                record_cleanup_error(Error::make(
                    ErrorCode::OsError,
                    "cannot determine whether owned game has exited",
                    GetLastError()));
            }
        }
        journal.stage = SessionStage::Failed;
        if (auto saved = save_journal(journal); !saved) {
            record_cleanup_error(saved.error());
        }
        if (!game_dead) {
            std::string message = failure.error.message;
            if (cleanup_failed) {
                message += "; cleanup failed: " + cleanup_error.message;
            }
            message += "; owned game death could not be confirmed; recovery "
                       "journal retained";
            context.stage = SessionStage::Failed;
            return std::unexpected(Error::make(ErrorCode::SessionFailed,
                                               std::move(message),
                                               cleanup_failed ? cleanup_error.os_code : 0));
        }
        if (game_has_run) {
            if (auto restored = restore_session_state(); !restored) {
                record_cleanup_error(restored.error());
            }
        }
        if (cleanup_failed) {
            context.stage = SessionStage::Failed;
            return std::unexpected(Error::make(
                ErrorCode::SessionFailed,
                failure.error.message + "; cleanup failed: " +
                    cleanup_error.message + "; recovery journal retained",
                cleanup_error.os_code));
        }
        if (auto cleared = clear_journal(); !cleared) {
            context.stage = SessionStage::Failed;
            return std::unexpected(cleared.error());
        }
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
        if (auto saved = save_journal(journal); !saved) {
            return std::unexpected(saved.error());
        }
        if (auto cleared = clear_journal(); !cleared) {
            return std::unexpected(cleared.error());
        }
        return std::unexpected(launch_plan.error());
    }
    auto launched =
        win32::spawn_suspended(launch_plan->executable, launch_plan->arguments,
                               launch_plan->working_directory,
                               priority_class(launch_plan->priority));
    if (!launched) {
        journal.stage = SessionStage::Failed;
        if (auto saved = save_journal(journal); !saved) {
            return std::unexpected(saved.error());
        }
        if (auto cleared = clear_journal(); !cleared) {
            return std::unexpected(cleared.error());
        }
        return std::unexpected(launched.error());
    }
    context.pid = launched->pid;
    journal.pid = launched->pid;
    journal.stage = SessionStage::Resolving;
    if (auto saved = save_journal(journal); !saved) {
        return finish_failed({&*launched, nullptr, false, saved.error()});
    }

    // ---- Resolving --------------------------------------------------------
    // Engine modules (UnityPlayer.dll etc.) only exist once the game's
    // loader has run. Main-module-only sessions snapshot right away; the
    // others resume, wait for the modules to appear, then re-suspend for
    // patching - external only, no remote thread, no injection.
    context.stage = SessionStage::Resolving;
    std::vector<scan::ModuleSnapshot> snapshots;
    std::vector<uintptr_t> bases;
    auto located = locate_modules(launched->process, *requirements);
    if (!located) {
        // Give the loader a chance, then re-suspend for a stable snapshot.
        game_has_run = true;
        if (auto resumed = win32::resume_process_threads(launched->pid); !resumed) {
            return finish_failed({&*launched, nullptr, false, resumed.error()});
        }
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
    journal.rollback.required = true;
    if (auto saved = save_journal(journal); !saved) {
        return finish_failed({&*launched, nullptr, game_has_run, saved.error()});
    }

    PatchContext patch_context{*resolved, request.profile,
                               bases.empty() ? 0 : bases.front(), *old_version};
    auto plan = adapter_.build_patch_plan(patch_context);
    if (!plan) {
        return finish_failed({&*launched, nullptr, game_has_run, plan.error()});
    }
    if (config_.verbose && plan->mobile_ui_diagnostic.has_value()) {
        const auto& d = *plan->mobile_ui_diagnostic;
        std::cout << "mobile-ui:\n"
                  << "  variant              " << d.variant << "\n"
                  << "  grph-class-global    "
                  << hex_address(d.grph_class_global) << "\n"
                  << "  grph-ui-offset       "
                  << hex_address(static_cast<uint32_t>(d.grph_ui_offset)) << "\n"
                  << "  grph-input-offset    "
                  << hex_address(static_cast<uint32_t>(d.grph_input_offset)) << "\n"
                  << "  gui-set              " << hex_address(d.func_gui_set)
                  << "\n"
                  << "  input-set            " << hex_address(d.func_input_set)
                  << "\n"
                  << "  lifecycle-hook       "
                  << hex_address(d.lifecycle_function_entry) << "\n"
                  << "  call-site-diagnostic "
                  << hex_address(d.lifecycle_call_disp_diagnostic) << "\n"
                  << "  mode                 upstream-function-entry\n";
    }
    patch::AppliedPatch applied;
    auto apply_result = patch::apply_patch_plan(launched->process, *plan);
    if (!apply_result) {
        return finish_failed({&*launched, nullptr, game_has_run, apply_result.error()});
    }
    applied = std::move(*apply_result);
    if (config_.verbose && plan->mobile_ui_diagnostic.has_value()) {
        const auto detour = std::find_if(
            applied.operations.begin(), applied.operations.end(),
            [](const auto& operation) {
                return operation.op.kind ==
                       PatchOperationKind::InstallFunctionEntryDetour;
            });
        if (detour != applied.operations.end()) {
            std::cout << "  hook-original        "
                      << hex_bytes(detour->original) << "\n";
        }
    }

    // ---- Running ----------------------------------------------------------
    // Past this point patched pages may execute: external rollback is no
    // longer safe, and everything in the process - patches included - dies
    // with it.
    // rollback.required stays true: even with patches live the recorded
    // persistent/display state still has to survive a launcher crash (Test D).
    context.stage = SessionStage::Running;
    journal.stage = SessionStage::Running;
    if (auto saved = save_journal(journal); !saved) {
        return finish_failed({&*launched, &applied, false, saved.error()});
    }

    const auto game_started = std::chrono::steady_clock::now();
    if (game_has_run) {
        if (auto resumed = win32::resume_process_threads(launched->pid); !resumed) {
            return finish_failed({&*launched, &applied, false, resumed.error()});
        }
    } else {
        if (auto resumed = win32::resume_thread(launched->thread); !resumed) {
            return finish_failed({&*launched, &applied, false, resumed.error()});
        }
        game_has_run = true;
    }

    // Game is running: keep the game's persisted settings desktop-shaped
    // while it may rewrite them mid-session (plan §9.2, event-driven).
    PersistentStateGuard guard;
    if (persistent_snapshot.has_value()) {
        if (auto started = guard.start(*persistent_snapshot); !started) {
            return finish_failed({&*launched, &applied, true, started.error()});
        }
    }

    // F6/F7 resident runtime: power-save foreground reactions and hotkeys.
    // With both disabled nothing is created - HoyoFlux then never touches
    // the fps channel while the game runs (the plan's regression gate).
    const bool power_save =
        request.profile.runtime.power_save == PowerSavePolicy::Enabled;
    const bool hotkeys = request.profile.runtime.hotkeys;
    runtime::RuntimeController controller;
    if (power_save || hotkeys) {
        runtime::RuntimeController::Config controller_config;
        controller_config.game_pid = launched->pid;
        controller_config.profile_fps = request.profile.runtime.fps;
        controller_config.power_save_fps = request.profile.runtime.power_save_fps;
        controller_config.power_save_enabled = power_save;
        controller_config.hotkeys_enabled = hotkeys;
        // The writer is bound to whatever fps channel the plan established:
        // the RemoteState slot (Genshin redirect) or the direct variable
        // (Star Rail write + mov flip).
        PatchPlan plan_copy = *plan;
        runtime::RuntimeController::FpsWriter writer =
            [&process = launched->process, &applied,
             &plan_copy](uint32_t fps) -> Result<void> {
            if (applied.runtime.base != 0) {
                return patch::write_remote_fps(process, applied.runtime.base,
                                               fps);
            }
            if (plan_copy.fps_direct_address.has_value()) {
                return patch::write_u32(process, *plan_copy.fps_direct_address,
                                        fps);
            }
            return std::unexpected(Error::make(
                ErrorCode::NotSupported,
                "no dynamic fps channel in the applied plan"));
        };
        if (auto started = controller.start(controller_config, writer);
            !started) {
            return finish_failed({&*launched, &applied, true, started.error()});
        }
    }

#if defined(HOYOFLUX_EXPERIMENTAL_MOBILE_UI)
    // B1 validation only: production builds compile out this polling loop.
    // Verbose experimental sessions sample once per second, emit only state
    // transitions, and stop as soon as the upstream-parity stub completes.
    if (config_.verbose && plan->mobile_ui_diagnostic.has_value()) {
        const auto detour = std::find_if(
            applied.operations.begin(), applied.operations.end(),
            [](const auto& operation) {
                return operation.op.kind ==
                       PatchOperationKind::InstallFunctionEntryDetour;
            });
        if (detour != applied.operations.end() && detour->allocated_base != 0) {
            const uintptr_t telemetry_address =
                detour->allocated_base +
                plan->mobile_ui_diagnostic->telemetry_offset;
            MobileUiTelemetry previous{};
            for (uint32_t sample = 0; sample < 60; ++sample) {
                const DWORD wait =
                    WaitForSingleObject(launched->process.get(), 1000);
                if (wait == WAIT_OBJECT_0) {
                    break;
                }
                if (wait == WAIT_FAILED) {
                    return finish_failed({
                        &*launched, &applied, true,
                        Error::make(ErrorCode::OsError,
                                    "Mobile UI validation wait failed",
                                    GetLastError())});
                }

                MobileUiTelemetry current{};
                std::span bytes(reinterpret_cast<std::byte*>(&current),
                                sizeof(current));
                auto read = patch::read_bytes(launched->process,
                                              telemetry_address, bytes);
                if (!read) {
                    std::cout << "mobile-ui runtime: telemetry read failed: "
                              << read.error().message << "\n";
                    break;
                }
                if (std::memcmp(&current, &previous, sizeof(current)) != 0) {
                    const auto yes_no = [](uint32_t value) {
                        return value != 0 ? "yes" : "no";
                    };
                    std::cout
                        << "mobile-ui runtime:\n"
                        << "  function-entry-hits "
                        << current.function_entry_hits << "\n"
                        << "  graph-ready       "
                        << yes_no(current.graph_ready) << "\n"
                        << "  ui-ready          " << yes_no(current.ui_ready)
                        << "\n"
                        << "  input-ready       "
                        << yes_no(current.input_ready) << "\n"
                        << "  gui-set-called    "
                        << yes_no(current.gui_set_called) << "\n"
                        << "  input-set-called  "
                        << yes_no(current.input_set_called) << "\n"
                        << "  self-unhooked     "
                        << yes_no(current.self_unhooked) << "\n"
                        << "  original-resumed  "
                        << yes_no(current.original_resumed) << "\n"
                        << "  completed         "
                        << yes_no(current.completed) << "\n";
                    previous = current;
                }
                if (current.completed != 0) {
                    break;
                }
            }
        }
    }
#endif

    const DWORD process_wait =
        WaitForSingleObject(launched->process.get(), INFINITE);
    if (process_wait != WAIT_OBJECT_0) {
        return finish_failed({
            &*launched, &applied, true,
            Error::make(ErrorCode::OsError, "game process wait failed",
                        GetLastError())});
    }
    context.game_runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - game_started)
            .count());
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(launched->process.get(), &exit_code)) {
        return finish_failed({&*launched, &applied, true,
                              Error::make(ErrorCode::OsError,
                                          "GetExitCodeProcess failed",
                                          GetLastError())});
    }
    context.process_exit_code = exit_code;

    // ---- Restoring / Completed -------------------------------------------
    context.stage = SessionStage::Restoring;
    controller.stop();
    guard.stop();
    // F2 primary restore: whatever the game persisted for its next launch
    // goes back to the pre-launch snapshot (Test C of the release gate).
    // Persistent-state exact restore + verification, then the physical-mode
    // fallback. Failure deliberately leaves the recovery journal intact.
    if (auto restored = restore_session_state(); !restored) {
        context.stage = SessionStage::Failed;
        return std::unexpected(restored.error());
    }
    if (auto cleared = clear_journal(); !cleared) {
        context.stage = SessionStage::Failed;
        return std::unexpected(cleared.error());
    }
    if (context.process_exit_code != 0) {
        context.stage = SessionStage::Failed;
        return std::unexpected(Error::make(
            ErrorCode::SessionFailed,
            "game exited with code " +
                std::to_string(context.process_exit_code) + " after " +
                std::to_string(context.game_runtime_ms) + " ms"));
    }
    if (request.profile.ui.mobile_ui && context.game_runtime_ms < 5000) {
        context.stage = SessionStage::Failed;
        return std::unexpected(Error::make(
            ErrorCode::SessionFailed,
            "game exited during experimental Mobile UI startup after " +
                std::to_string(context.game_runtime_ms) + " ms"));
    }
    context.stage = SessionStage::Completed;
    return context;
}

}  // namespace hoyoflux::session
