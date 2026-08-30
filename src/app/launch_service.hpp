#pragma once

#include "domain/error.hpp"
#include "domain/game.hpp"
#include "domain/profile.hpp"
#include "game/game_adapter.hpp"
#include "profile/config.hpp"
#include "session/session_engine.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hoyoflux::app {

enum class InvocationMode { OneClick, Cli };

[[nodiscard]] constexpr InvocationMode select_invocation_mode(int argc) {
    return argc == 1 ? InvocationMode::OneClick : InvocationMode::Cli;
}

struct LaunchOptions {
    GameId game{GameId::Genshin};
    std::string profile{"auto"};
    game::Region region{game::Region::Auto};
    std::optional<std::filesystem::path> exe;
    std::vector<std::wstring> passthrough;
    std::optional<uint32_t> fps_override;
    std::optional<bool> mobile_ui_override;
    std::optional<float> dpi_override;
    bool verbose{false};
};

struct ResolvedLaunch {
    Profile profile;
    std::optional<profile::AutoProfileDecision> auto_decision;
};

struct LaunchOutcome {
    ResolvedLaunch resolved;
    SessionContext session;
};

[[nodiscard]] std::filesystem::path config_path();
Result<ResolvedLaunch> resolve_launch(const profile::Config& config,
                                      const LaunchOptions& options);
Result<LaunchOutcome> run_launch(const profile::Config& config,
                                 const LaunchOptions& options);

}  // namespace hoyoflux::app
