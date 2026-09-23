#pragma once

#include "domain/error.hpp"
#include "domain/game.hpp"
#include "domain/profile.hpp"
#include "game/game_adapter.hpp"
#include "profile/config.hpp"
#include "session/session_engine.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hoyoflux::app {

struct LaunchOptions {
    GameId game{GameId::Genshin};
    std::string profile{"auto"};
    game::Region region{game::Region::Auto};
    std::filesystem::path journal_path;
};

struct ResolvedLaunch {
    Profile profile;
    std::optional<profile::AutoProfileDecision> auto_decision;
};

struct LaunchOutcome {
    ResolvedLaunch resolved;
    SessionContext session;
};

Result<ResolvedLaunch> resolve_launch(const profile::Config& config,
                                      const LaunchOptions& options);
Result<LaunchOutcome> run_launch(const profile::Config& config,
                                 const LaunchOptions& options);
Result<LaunchOutcome> run_resolved_launch(const LaunchOptions& options,
                                          ResolvedLaunch resolved,
                                          std::function<void()> on_preflight_pass = {});

}  // namespace hoyoflux::app
