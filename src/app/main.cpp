// HoyoFlux CLI (A10). Quiet by default: one line per outcome, details with
// --verbose. Commands:
//   launch <genshin|starrail> [options]
//   profile list | show <id> | path
//   doctor
//   recover
//   version

#include "app/doctor.hpp"
#include "app/launch_service.hpp"
#include "domain/error.hpp"
#include "domain/game.hpp"
#include "domain/launch_request.hpp"
#include "game/game_adapter.hpp"
#include "game/genshin/signatures.hpp"
#include "game/starrail/signatures.hpp"
#include "platform/win32/display.hpp"
#include "platform/win32/console.hpp"
#include "platform/win32/elevation.hpp"
#include "platform/win32/notification.hpp"
#include "platform/win32/privilege.hpp"
#include "platform/win32/process.hpp"
#include "platform/win32/registry.hpp"
#include "profile/config.hpp"
#include "session/journal.hpp"
#include "session/session_engine.hpp"
#include "version.hpp"

#include <CLI/CLI.hpp>

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace hoyoflux;

namespace {

// Console-safe narrowing of registry paths (the Genshin CN key is
// non-ASCII). UTF-8 output; a legacy console may need `chcp 65001`.
std::string narrow(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                         static_cast<int>(wide.size()), nullptr,
                                         0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), char{});
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(size), wchar_t{});
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                        static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

struct BootstrapArguments {
    bool internal_elevated{false};
    std::optional<DWORD> console_owner;
    std::vector<std::wstring> public_args;
};

std::optional<DWORD> parse_console_owner(std::wstring_view argument) {
    if (!argument.starts_with(win32::kConsoleOwnerArgumentPrefix)) {
        return std::nullopt;
    }
    const auto digits = argument.substr(win32::kConsoleOwnerArgumentPrefix.size());
    if (digits.empty()) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (const wchar_t digit : digits) {
        if (digit < L'0' || digit > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<uint64_t>(digit - L'0');
        if (value > std::numeric_limits<DWORD>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<DWORD>(value);
}

BootstrapArguments split_bootstrap_arguments(
    const std::vector<std::wstring>& arguments) {
    BootstrapArguments result;
    result.public_args.reserve(arguments.size());
    bool passthrough = false;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        if (i != 0 && !passthrough &&
            argument == win32::kInternalElevatedArgument) {
            result.internal_elevated = true;
            continue;
        }
        if (i != 0 && !passthrough &&
            argument.starts_with(win32::kConsoleOwnerArgumentPrefix)) {
            result.console_owner = parse_console_owner(argument);
            continue;
        }
        if (i != 0 && argument == L"--") {
            passthrough = true;
        }
        result.public_args.push_back(argument);
    }
    return result;
}

bool contains_launch_command(const std::vector<std::wstring>& arguments) {
    for (size_t i = 1; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        if (argument == L"--verbose" || argument == L"-v") {
            continue;
        }
        if (!argument.empty() && argument.front() == L'-') {
            continue;
        }
        return argument == L"launch";
    }
    return false;
}

std::wstring profile_label(std::string_view id) {
    if (id == "ipad") return L"iPad";
    if (id == "xiaomi") return L"Xiaomi";
    if (id == "desktop" || id == "starrail_desktop") return L"Desktop";
    return widen(id);
}

const char* process_priority_label(ProcessPriority priority) {
    switch (priority) {
    case ProcessPriority::Realtime:
        return "realtime";
    case ProcessPriority::High:
        return "high";
    case ProcessPriority::AboveNormal:
        return "above_normal";
    case ProcessPriority::Normal:
        return "normal";
    case ProcessPriority::BelowNormal:
        return "below_normal";
    }
    return "unknown";
}

std::wstring launch_notification_body(const app::ResolvedLaunch& resolved) {
    const auto& selected = resolved.profile;
    std::wstring body = resolved.auto_decision ? L"已自动选择 " : L"正在使用 ";
    body += profile_label(selected.id);
    body += selected.game == GameId::Genshin ? L"\n原神" : L"\n崩坏：星穹铁道";
    if (selected.render.resolution) {
        body += L" · " + std::to_wstring(selected.render.resolution->width) +
                L"×" + std::to_wstring(selected.render.resolution->height);
    }
    body += L" · " + std::to_wstring(selected.runtime.fps) + L" FPS";
    if (selected.ui.mobile_ui) {
        body += L" · Mobile UI";
    }
    return body;
}

void surface_one_click_error(std::wstring body) {
    auto result = win32::notify(L"HoyoFlux", body,
                                win32::NotificationKind::Error);
    if (!result) {
        MessageBoxW(nullptr, body.c_str(), L"HoyoFlux", MB_OK | MB_ICONERROR);
    }
    win32::cleanup_notifications();
}

// Single version source: generated by CMake (plan §1). Currently a
// prerelease - the tree must not present itself as final 1.0.0.
constexpr const char* kVersion = HOYOFLUX_VERSION_STRING;

GameId parse_game_id(const std::string& text) {
    if (text == "genshin") {
        return GameId::Genshin;
    }
    return GameId::StarRail;  // CLI11 restricts the input beforehand
}

int cmd_launch(const std::string& game_text, const std::string& profile_name,
               const std::optional<uint32_t>& fps,
               const std::optional<bool>& mobile_ui,
               const std::optional<float>& dpi_scale,
               const std::string& region_text, const std::optional<std::string>& exe,
               const std::vector<std::wstring>& passthrough, bool verbose) {
    app::LaunchOptions options;
    options.game = parse_game_id(game_text);
    options.profile = profile_name;
    options.passthrough = passthrough;
    options.fps_override = fps;
    options.mobile_ui_override = mobile_ui;
    options.dpi_override = dpi_scale;
    options.verbose = verbose;
    if (region_text == "cn") {
        options.region = game::Region::Cn;
    } else if (region_text == "global") {
        options.region = game::Region::Global;
    }
    if (exe) {
        options.exe = std::filesystem::path(widen(*exe));
    }
    auto config = profile::load_config(app::config_path());
    if (!config) {
        std::cout << "error: config " << config.error().message << "\n";
        return 1;
    }
    const auto started = std::chrono::steady_clock::now();
    if (verbose) {
        std::cout << "session: game=" << to_string(options.game)
                  << " requested_profile=" << options.profile
                  << " passthrough=" << passthrough.size() << " arg(s)\n";
    }
    auto outcome = app::run_launch(*config, options);
    const auto seconds = std::chrono::duration_cast<std::chrono::duration<double>>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    if (!outcome) {
        std::cout << "error: " << to_string(outcome.error().code) << " "
                  << outcome.error().message << "\n";
        return 1;
    }
    std::cout << "session " << outcome->session.id << " completed: game="
              << to_string(options.game) << " profile="
              << outcome->resolved.profile.id << " pid=" << outcome->session.pid
              << " exit_code=" << outcome->session.process_exit_code
              << " game_runtime=" << (outcome->session.game_runtime_ms / 1000.0)
              << "s duration=" << seconds << "s\n";
    return 0;
}

int cmd_profile_list() {
    auto config = profile::load_config(app::config_path());
    if (!config) {
        std::cout << "error: config " << config.error().message << "\n";
        return 1;
    }
    for (const auto& profile : config->profiles) {
        std::cout << profile.id << "  game=" << to_string(profile.game)
                  << " fps=" << profile.runtime.fps
                  << " mobile_ui=" << (profile.ui.mobile_ui ? "yes" : "no")
                  << (profile.id == config->default_profile ? "  [default]" : "")
                  << "\n";
    }
    return 0;
}

int cmd_profile_show(const std::string& id) {
    auto config = profile::load_config(app::config_path());
    if (!config) {
        std::cout << "error: config " << config.error().message << "\n";
        return 1;
    }
    auto profile = profile::find_profile(*config, id);
    if (!profile) {
        std::cout << "error: profile " << profile.error().message << "\n";
        return 1;
    }
    const auto& p = *profile;
    std::cout << "profile:           " << p.id << "\n";
    std::cout << "game:              " << to_string(p.game) << "\n";
    std::cout << "\nMatch\n";
    std::cout << "  auto_select:     " << (p.match.auto_select ? "yes" : "no")
              << "\n";
    if (p.match.device_name) {
        std::cout << "  device_name:     " << narrow(*p.match.device_name) << "\n";
    }
    if (p.match.resolution) {
        std::cout << "  resolution:      " << p.match.resolution->width << "x"
                  << p.match.resolution->height << "\n";
    }
    if (p.match.aspect_ratio) {
        std::cout << "  aspect_ratio:    " << *p.match.aspect_ratio << "\n";
    }
    if (p.match.portrait.has_value()) {
        std::cout << "  portrait:         " << (*p.match.portrait ? "yes" : "no")
                  << "\n";
    }
    std::cout << "  priority:        " << p.match.priority << "\n";

    std::cout << "\nRender\n";
    if (p.render.resolution) {
        std::cout << "  resolution:      " << p.render.resolution->width << "x"
                  << p.render.resolution->height << "\n";
    } else {
        std::cout << "  resolution:      unset\n";
    }
    if (p.render.fullscreen) {
        const char* fullscreen = *p.render.fullscreen == FullscreenMode::Exclusive
                                     ? "exclusive"
                                     : (*p.render.fullscreen ==
                                                FullscreenMode::Windowed
                                            ? "windowed"
                                            : "borderless");
        std::cout << "  fullscreen:      " << fullscreen << "\n";
    } else {
        std::cout << "  fullscreen:      unset\n";
    }
    std::cout << "  persistence:     "
              << (p.render.persistence == ResolutionPersistence::Persistent
                      ? "persistent"
                      : "session")
              << "\n";
    if (p.render.monitor) {
        std::cout << "  monitor:          " << *p.render.monitor << "\n";
    }

    std::cout << "\nRuntime\n";
    std::cout << "  fps:             " << p.runtime.fps << "\n";
    std::cout << "  power_save:      "
              << (p.runtime.power_save == PowerSavePolicy::Enabled ? "enabled"
                                                                   : "disabled")
              << " at " << p.runtime.power_save_fps << " fps\n";
    std::cout << "  priority:        " << process_priority_label(p.runtime.priority)
              << "\n";
    std::cout << "  hotkeys:         " << (p.runtime.hotkeys ? "yes" : "no")
              << "\n";

    std::cout << "\nUI\n";
    std::cout << "  mobile_ui:       " << (p.ui.mobile_ui ? "yes" : "no") << "\n";
    if (p.ui.dpi_scale) {
        std::cout << "  dpi_scale:       " << *p.ui.dpi_scale << "\n";
    } else {
        std::cout << "  dpi_scale:       unset\n";
    }
    return 0;
}

int cmd_profile_match(const std::string& game_text, bool verbose) {
    const auto game = parse_game_id(game_text);
    auto config = profile::load_config(app::config_path());
    if (!config) {
        std::cout << "error: config " << config.error().message << "\n";
        return 1;
    }
    auto displays = win32::enumerate_displays();
    if (!displays) {
        std::cout << "error: displays " << displays.error().message << "\n";
        return 1;
    }
    auto decision = profile::resolve_auto_profile(*config, game, *displays);
    if (!decision) {
        std::cout << "error: " << to_string(decision.error().code) << " "
                  << decision.error().message << "\n";
        return 1;
    }
    std::cout << "Auto profile evaluation: " << to_string(game)
              << "\n\nDisplays\n";
    for (const auto& display : decision->displays) {
        std::cout << "  " << narrow(display.info.device_name) << "\n"
                  << "    mode:       " << display.resolution.width << "x"
                  << display.resolution.height;
        if (display.refresh_rate != 0) {
            std::cout << "@" << display.refresh_rate;
        }
        std::cout << "\n    primary:    "
                  << (display.info.is_primary ? "yes" : "no") << "\n";
    }
    std::cout << "\nCandidates\n";
    for (const auto& candidate : decision->candidates) {
        std::cout << "  " << candidate.profile_id;
        if (candidate.specificity < 0) {
            std::cout << "  no match\n";
        } else {
            std::cout << "\n    display:     ";
            bool printed_name = false;
            if (candidate.display_index) {
                for (const auto& display : decision->displays) {
                    if (display.info.index == *candidate.display_index) {
                        std::cout << narrow(display.info.device_name);
                        printed_name = true;
                        break;
                    }
                }
            }
            if (!printed_name) {
                std::cout << "unknown";
            }
            std::cout << "\n    specificity: " << candidate.specificity
                      << "\n    priority:    " << candidate.priority << "\n";
            if (verbose && candidate.display_index) {
                std::cout << "    display_index: " << *candidate.display_index
                          << "\n";
            }
        }
    }
    std::cout << "\nSelected\n  " << decision->profile.id;
    if (decision->used_fallback) {
        std::cout << " [fallback]";
    }
    std::cout << "\n";
    return 0;
}

// Read-only dump of a game's persistent display settings (plan §7.2). This
// is the A/B experiment tool: dump before a session, dump after, diff the
// output. It never writes anything.
int cmd_state_dump(const std::string& game_text) {
    const auto game = parse_game_id(game_text);
    auto adapter = game::make_adapter(game);

    std::cout << "persistent-state roots for " << to_string(game) << ":\n";
    for (const auto& root : adapter->persistent_state_roots()) {
        auto exists = win32::registry_key_exists(root);
        std::cout << "  " << (exists && *exists ? "[x] " : "[ ] ")
                  << "HKCU\\" << narrow(root) << "\n";
        if (!exists || !*exists) {
            continue;
        }
        auto values = win32::read_registry_values(root);
        if (!values) {
            std::cout << "      error: " << values.error().message << "\n";
            continue;
        }
        size_t shown = 0;
        for (const auto& value : *values) {
            if (value.name.rfind(L"Screenmanager", 0) != 0) {
                continue;
            }
            std::cout << "      " << std::string(value.name.begin(), value.name.end());
            if (value.type == REG_DWORD && value.data.size() == 4) {
                uint32_t dword = 0;
                std::memcpy(&dword, value.data.data(), sizeof(dword));
                std::cout << " = " << dword;
            }
            std::cout << "\n";
            ++shown;
        }
        if (shown == 0) {
            std::cout << "      (no Screenmanager values)\n";
        }
    }
    return 0;
}

int cmd_recover() {
    auto adapter = game::make_adapter(GameId::Genshin);
    session::SessionEngine engine(*adapter);
    auto action = engine.recover();
    if (!action) {
        std::cout << "error: recover " << action.error().message << "\n";
        return 1;
    }
    switch (*action) {
    case session::RecoveryAction::None:
        std::cout << "no active-session journal found\n";
        return 0;
    case session::RecoveryAction::Recovered:
        std::cout << "recovered: recorded state restored and verified\n";
        return 0;
    case session::RecoveryAction::GameStillRunning:
        std::cout << "journal references a live process; nothing done\n";
        return 1;
    case session::RecoveryAction::RecoveryFailed:
        std::cout << "recovery FAILED: recorded state could not be restored "
                     "or verified; the journal is kept for a retry\n";
        return 2;
    }
    return 0;
}

int run_one_click() {
    auto config = profile::load_config(app::config_path());
    if (!config) {
        surface_one_click_error(L"启动失败\n" + widen(config.error().message));
        return 1;
    }
    app::LaunchOptions options;
    options.game = config->launcher.game;
    options.profile = config->launcher.profile;
    switch (config->launcher.region) {
    case profile::LauncherRegion::Auto:
        options.region = game::Region::Auto;
        break;
    case profile::LauncherRegion::Cn:
        options.region = game::Region::Cn;
        break;
    case profile::LauncherRegion::Global:
        options.region = game::Region::Global;
        break;
    }
    auto resolved = app::resolve_launch(*config, options);
    if (!resolved) {
        surface_one_click_error(L"启动失败\n" +
                                widen(resolved.error().message));
        return 1;
    }
    if (config->launcher.notifications) {
        win32::notify_best_effort(win32::notify, L"HoyoFlux",
                                  launch_notification_body(*resolved),
                                  win32::NotificationKind::Info);
        win32::cleanup_notifications();
    }
    auto outcome = app::run_resolved_launch(options, std::move(*resolved));
    if (!outcome) {
        surface_one_click_error(L"启动失败\n" +
                                widen(outcome.error().message));
        return 1;
    }
    if (config->launcher.notifications) {
        win32::notify_best_effort(win32::notify, L"HoyoFlux",
                                  L"游戏已结束\n会话设置已恢复",
                                  win32::NotificationKind::Success);
        win32::cleanup_notifications();
    }
    return 0;
}

}  // namespace

int application_main() {
    // Split off the game-argument passthrough BEFORE CLI11 sees the command
    // line (plan §4): for `launch`, the first `--` after the launch token
    // marks the start of verbatim game arguments. CLI11 has no per-subcommand
    // `--` support, so this pre-split IS the passthrough contract.
    int wide_argc = 0;
    LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (wide_argv == nullptr) {
        std::cerr << "error: CommandLineToArgvW failed\n";
        return 1;
    }
    std::vector<std::wstring> wide_owned(wide_argv, wide_argv + wide_argc);
    LocalFree(wide_argv);

    const auto bootstrap = split_bootstrap_arguments(wide_owned);
    const bool one_click = bootstrap.public_args.size() == 1;
    const bool launch_command = contains_launch_command(bootstrap.public_args);
    bool console_attached = false;
    if (!one_click) {
        console_attached = bootstrap.internal_elevated && bootstrap.console_owner
                               ? win32::attach_console(*bootstrap.console_owner)
                               : win32::attach_parent_console();
    }

    if ((one_click || launch_command) && !win32::is_elevated()) {
        if (bootstrap.internal_elevated) {
            if (one_click) {
                return 1;
            }
            std::cerr << "error: elevated child is not elevated\n";
            return 1;
        }

        std::vector<std::wstring> child_arguments(
            bootstrap.public_args.begin() + 1, bootstrap.public_args.end());
        auto marker_position = child_arguments.end();
        for (auto iterator = child_arguments.begin();
             iterator != child_arguments.end(); ++iterator) {
            if (*iterator == L"--") {
                marker_position = iterator;
                break;
            }
        }
        marker_position = child_arguments.insert(
            marker_position, std::wstring(win32::kInternalElevatedArgument));
        if (console_attached) {
            child_arguments.insert(
                marker_position + 1,
                std::wstring(win32::kConsoleOwnerArgumentPrefix) +
                std::to_wstring(GetCurrentProcessId()));
        }
        win32::ElevationResult elevation_result =
            win32::ElevationResult::Completed;
        auto child = win32::relaunch_elevated_and_wait(child_arguments,
                                                        &elevation_result);
        if (!child) {
            if (elevation_result == win32::ElevationResult::Cancelled) {
                if (!one_click) {
                    std::cerr << "error: elevation cancelled\n";
                    return 1;
                }
                return 0;
            }
            if (one_click) {
                surface_one_click_error(L"启动失败\n" + widen(child.error().message));
            } else {
                std::cerr << "error: " << to_string(child.error().code) << " "
                          << child.error().message << "\n";
            }
            return 1;
        }
        return *child;
    }

    if (one_click) {
        return run_one_click();
    }

    std::vector<std::wstring> passthrough;
    std::vector<std::string> owned;
    owned.reserve(bootstrap.public_args.size());
    for (const auto& arg : bootstrap.public_args) {
        owned.push_back(narrow(arg));
    }
    std::vector<char*> parsed_argv;
    parsed_argv.reserve(owned.size());
    int launch_pos = -1;
    for (int i = 1; i < static_cast<int>(bootstrap.public_args.size()); ++i) {
        if (owned[i] == "launch") {
            launch_pos = i;
            break;
        }
    }
    bool passthrough_mode = false;
    for (int i = 0; i < static_cast<int>(bootstrap.public_args.size()); ++i) {
        if (passthrough_mode) {
            passthrough.push_back(bootstrap.public_args[i]);
            continue;
        }
        if (launch_pos >= 0 && i > launch_pos && owned[i] == "--") {
            passthrough_mode = true;
            continue;
        }
        parsed_argv.push_back(owned[i].data());
    }

    CLI::App app{"HoyoFlux " + std::string(kVersion) +
                 " - session launcher for HoYoverse PC games"};
    app.set_version_flag("--version", kVersion);
    app.require_subcommand(1, 1);

    bool verbose = false;
    int exit_code = 0;
    app.add_flag("--verbose", verbose, "show module/signature detail");

    std::string launch_game;
    std::string profile_name;
    std::optional<uint32_t> fps;
    std::optional<bool> mobile_ui_flag;
    std::optional<float> dpi_scale;
    std::string region = "auto";
    std::optional<std::string> exe;
    auto* launch = app.add_subcommand("launch", "launch a game session")
                       ->group("Commands");
    launch->callback([&] {
        // Passthrough was split from the native wide command line, so every
        // game argument remains lossless regardless of the active code page.
        exit_code = cmd_launch(launch_game, profile_name, fps, mobile_ui_flag,
                               dpi_scale, region, exe, passthrough, verbose);
    });
    launch->add_option("game", launch_game, "genshin | starrail")
        ->check(CLI::IsMember({"genshin", "starrail"}))
        ->required();
    launch->add_option("--profile", profile_name, "profile id or 'auto'");
    launch->add_option("--fps", fps, "override profile fps");
    launch->add_flag("--mobile-ui{true},--no-mobile-ui{false}", mobile_ui_flag,
                     "override mobile UI");
    launch->add_option("--dpi", dpi_scale, "override DPI scale");
    launch->add_option("--region", region, "auto | cn | global")
        ->check(CLI::IsMember({"auto", "cn", "global"}));
    launch->add_option("--exe", exe, "explicit game executable path");
    launch->add_flag("-v,--verbose", verbose, "verbose output");

    auto* profile_cmd = app.add_subcommand("profile", "manage profiles")
                            ->require_subcommand()
                            ->group("Commands");
    profile_cmd->add_subcommand("list", "list profiles")
        ->callback([&] { exit_code = cmd_profile_list(); });
    std::string show_id;
    profile_cmd
        ->add_subcommand("show", "show one profile")
        ->callback([&] { exit_code = cmd_profile_show(show_id); })
        ->add_option("id", show_id, "profile id")
        ->required();
    profile_cmd->add_subcommand("path", "print the config file path")
        ->callback([] {
            std::cout << app::config_path().string() << "\n";
            return 0;
        });
    std::string match_game;
    profile_cmd->add_subcommand("match", "explain automatic profile selection")
        ->callback([&] { exit_code = cmd_profile_match(match_game, verbose); })
        ->add_option("game", match_game, "genshin | starrail")
        ->check(CLI::IsMember({"genshin", "starrail"}))
        ->required();

    app.add_subcommand("doctor", "diagnose the environment (read-only)")
        ->callback([&] { exit_code = app::run_doctor(verbose); })
        ->group("Commands");
    app.add_subcommand("version", "print the version")
        ->callback(
            [] {
                std::cout << "HoyoFlux " << kVersion << "\n";
            })
        ->group("Commands");
    app.add_subcommand("recover", "clean a stale session journal")
        ->callback([&] { exit_code = cmd_recover(); })
        ->group("Commands");
    std::string dump_game;
    app.add_subcommand("state-dump",
                       "print a game's persistent display settings (read-only)")
        ->callback([&] { exit_code = cmd_state_dump(dump_game); })
        ->group("Commands")
        ->add_option("game", dump_game, "genshin | starrail")
        ->check(CLI::IsMember({"genshin", "starrail"}))
        ->required();

    try {
        app.parse(static_cast<int>(parsed_argv.size()), parsed_argv.data());
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }
    return exit_code;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return application_main();
}
