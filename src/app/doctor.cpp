// Doctor implementation (F12, plan section 21): locate each game, read its
// version, resolve signatures live when the game happens to be running, and
// report the F0 capability model. Read-only: no patching, no spawning, no
// state changes.

#include "app/doctor.hpp"

#include "domain/capability.hpp"
#include "domain/profile.hpp"
#include "game/game_adapter.hpp"
#include "platform/win32/display.hpp"
#include "platform/win32/pe.hpp"
#include "platform/win32/privilege.hpp"
#include "platform/win32/process.hpp"
#include "platform/win32/registry.hpp"
#include "profile/config.hpp"
#include "scan/module_snapshot.hpp"
#include "session/journal.hpp"
#include "version.hpp"

#include <CLI/CLI.hpp>

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace hoyoflux::app {
namespace {

using namespace hoyoflux;

// The CLI config lives next to the state directory the journal owns.
std::filesystem::path config_path() {
    return session::journal_path().parent_path().parent_path() / "config.toml";
}

// Console-safe narrowing of registry paths (the Genshin CN key is
// non-ASCII). UTF-8 output; a legacy console may need `chcp 65001`.
std::string narrow(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                         static_cast<int>(wide.size()), nullptr,
                                         0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), size, nullptr, nullptr);
    return utf8;
}

int g_failures = 0;

void check(bool ok, std::string_view label, std::string_view detail) {
    std::cout << (ok ? "[ OK ] " : "[FAIL] ") << label;
    if (!detail.empty()) {
        std::cout << ": " << detail;
    }
    std::cout << "\n";
    if (!ok) {
        ++g_failures;
    }
}

void check_system() {
    OSVERSIONINFOEXW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    char windows_detail[64] = "unknown";
    if (rtl_get_version && rtl_get_version(&info) == 0) {
        std::snprintf(windows_detail, sizeof(windows_detail), "%lu.%lu.%lu",
                      info.dwMajorVersion, info.dwMinorVersion,
                      info.dwBuildNumber);
    }
    check(info.dwMajorVersion >= 10, "Windows", windows_detail);
    check(win32::is_elevated(), "Administrator",
          win32::is_elevated() ? "elevated" : "not elevated - launch may fail");
}

void check_config() {
    auto config = profile::load_config(config_path());
    if (config) {
        check(true, "config.toml",
              std::to_string(config->profiles.size()) + " profiles at " +
                  config_path().string());
    } else {
        check(false, "config.toml", config.error().message);
    }
}

void check_journal() {
    auto journal = session::load_journal();
    if (!journal) {
        check(false, "journal", journal.error().message);
    } else if (!journal->has_value()) {
        check(true, "journal", "none (clean)");
    } else {
        const auto& active = **journal;
        const bool alive =
            active.pid != 0 && win32::is_process_running(active.pid);
        check(!alive, "journal",
              alive ? "ACTIVE session=" + active.session_id +
                          " pid=" + std::to_string(active.pid)
                    : "stale journal present - run `hoyoflux recover`");
    }
}

void check_displays() {
    if (auto displays = win32::enumerate_displays(); displays) {
        std::string summary;
        for (const auto& display : *displays) {
            if (!display.is_attached) {
                continue;
            }
            auto settings = win32::query_current_settings(display.device_name);
            if (settings) {
                summary += std::filesystem::path(display.device_name).string() +
                           " " + std::to_string(settings->width) + "x" +
                           std::to_string(settings->height) + "@" +
                           std::to_string(settings->refresh_rate) + "  ";
            }
        }
        check(true, "displays", summary);
    } else {
        check(false, "displays", displays.error().message);
    }
}

// Per game: install, version, persistent roots, capabilities, and - when the
// game is currently running - live signature resolution.
void check_game(GameId game) {
    auto adapter = game::make_adapter(game);
    const std::string game_name = std::string(to_string(game));

    auto install = adapter->locate_installation(game::Region::Auto);
    if (!install) {
        check(false, game_name, install.error().message);
        return;
    }
    check(true, game_name, install->exe_path.string() +
                               (install->is_cn ? " (CN)" : " (Global)"));

    auto old_version = adapter->is_old_version(*install);
    if (old_version) {
        check(true, game_name + " version",
              *old_version ? "old (engine in UnityPlayer.dll)"
                           : "modern (merged exe)");
    } else {
        check(false, game_name + " version", old_version.error().message);
    }

    // Persistent-state roots (read-only existence + value counts).
    for (const auto& root : adapter->persistent_state_roots()) {
        auto exists = win32::registry_key_exists(root);
        if (exists && *exists) {
            auto values = win32::read_registry_values(root);
            size_t screenmanager = 0;
            if (values) {
                for (const auto& value : *values) {
                    if (value.name.rfind(L"Screenmanager", 0) == 0) {
                        ++screenmanager;
                    }
                }
            }
            std::cout << "       root HKCU\\" << narrow(root)
                      << ": " << screenmanager << " Screenmanager value(s)\n";
        } else {
            std::cout << "       root HKCU\\"
                      << narrow(root) << ": absent\n";
        }
    }

    // Capability report (F0 model, probe profile = fps only).
    const Profile probe;
    const auto report = adapter->capabilities(*install, probe);
    std::cout << "     capabilities:\n";
    for (const auto& entry : report.entries) {
        std::cout << "       " << std::left << std::setw(24) << to_string(entry.capability)
                  << to_string(entry.status);
        if (entry.status == CapabilityStatus::Unsupported &&
            !entry.reason.empty()) {
            std::cout << " - " << entry.reason;
        }
        std::cout << "\n";
    }

    // Live signature freshness: only possible when the game is running
    // (doctor never spawns or suspends anything).
    const auto exe_name = install->exe_path.filename().wstring();
    auto running = win32::find_process(exe_name);
    if (!running || !running->has_value()) {
        std::cout << "     signatures: game not running - live resolution "
                     "happens per launch (run `hoyoflux launch --verbose`)\n";
        return;
    }
    std::cout << "     signatures (live, pid " << (*running)->pid << "):\n";
    auto process =
        win32::open_process((*running)->pid, PROCESS_VM_READ | PROCESS_QUERY_INFORMATION);
    if (!process) {
        std::cout << "       cannot open the game process read-only\n";
        return;
    }
    auto requirements = adapter->module_requirements(*install, probe);
    if (!requirements) {
        std::cout << "       module requirements unavailable\n";
        return;
    }
    std::vector<scan::ModuleSnapshot> snapshots;
    for (const auto& requirement : requirements->modules) {
        auto base = requirement.module.empty()
                        ? scan::remote_module_base(*process)
                        : scan::remote_module_base(*process, requirement.module);
        if (!base) {
            std::cout << "       module not found: "
                      << (requirement.module.empty() ? "<main>"
                                                     : requirement.module)
                      << "\n";
            continue;
        }
        std::vector<std::string_view> sections(requirement.sections.begin(),
                                               requirement.sections.end());
        auto snapshot = scan::snapshot_module(*process, *base, sections);
        if (snapshot) {
            snapshots.push_back(std::move(*snapshot));
        }
    }
    auto resolved = adapter->resolve_signatures(snapshots);
    if (!resolved) {
        std::cout << "       resolution failed: " << resolved.error().message
                  << "\n";
        ++g_failures;
        return;
    }
    for (const auto& entry : *resolved) {
        check(entry.resolved, game_name + " sig " + std::string(entry.id),
              entry.resolved ? "resolved" : "MISSING for this game version");
    }
}

}  // namespace

int run_doctor(bool /*verbose*/) {
    std::cout << "HoyoFlux Doctor " << HOYOFLUX_VERSION_STRING << "\n\n";

    std::cout << "System\n";
    check_system();

    std::cout << "\nGames\n";
    check_game(GameId::Genshin);
    check_game(GameId::StarRail);

    std::cout << "\nState\n";
    check_config();
    check_journal();
    check_displays();

    std::cout << "\nDoctor is read-only: it never patches a game.\n";
    return g_failures == 0 ? 0 : 1;
}

}  // namespace hoyoflux::app
