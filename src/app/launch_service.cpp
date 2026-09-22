#include "app/launch_service.hpp"

#include "domain/launch_request.hpp"
#include "platform/win32/display.hpp"

namespace hoyoflux::app {

Result<ResolvedLaunch> resolve_launch(const profile::Config& config,
                                      const LaunchOptions& options) {
    ResolvedLaunch resolved;
    if (options.profile == "auto") {
        auto displays = win32::enumerate_displays();
        if (!displays) {
            return std::unexpected(displays.error());
        }
        auto decision = profile::resolve_auto_profile(config, options.game,
                                                      *displays);
        if (!decision) {
            return std::unexpected(decision.error());
        }
        resolved.profile = decision->profile;
        resolved.auto_decision = std::move(*decision);
    } else if (options.profile.empty()) {
        const std::string& per_game = options.game == GameId::Genshin
                                          ? config.genshin_default
                                          : config.starrail_default;
        const std::string& id = !per_game.empty() ? per_game
                                                  : config.default_profile;
        if (id.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::ProfileNotFound,
                "no profile given and no default profile set in config.toml"));
        }
        auto found = profile::find_profile(config, id);
        if (!found) {
            return std::unexpected(found.error());
        }
        resolved.profile = std::move(*found);
    } else {
        auto found = profile::find_profile(config, options.profile);
        if (!found) {
            return std::unexpected(found.error());
        }
        resolved.profile = std::move(*found);
    }
    if (resolved.profile.game != options.game) {
        return std::unexpected(Error::make(
            ErrorCode::ProfileInvalid,
            "profile '" + resolved.profile.id + "' belongs to game '" +
                std::string(to_string(resolved.profile.game)) + "'"));
    }
    return resolved;
}

Result<LaunchOutcome> run_launch(const profile::Config& config,
                                 const LaunchOptions& options) {
    auto resolved = resolve_launch(config, options);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return run_resolved_launch(options, std::move(*resolved));
}

Result<LaunchOutcome> run_resolved_launch(const LaunchOptions& options,
                                          ResolvedLaunch resolved,
                                          std::function<void()> on_preflight_pass) {
    LaunchRequest request;
    request.game = options.game;
    request.profile = resolved.profile;
    request.exe_override = resolved.profile.executable;
    request.game_args = resolved.profile.arguments;

    session::SessionConfig session_config;
    session_config.region = options.region;
    session_config.journal_path = options.journal_path;
    auto adapter = game::make_adapter(options.game);
    session::SessionEngine engine(*adapter, session_config);
    auto lease = session::SessionLease::acquire();
    if (!lease) {
        return std::unexpected(lease.error());
    }
    if (auto preflight = engine.preflight(*lease); !preflight) {
        return std::unexpected(preflight.error());
    }
    if (on_preflight_pass) {
        on_preflight_pass();
    }
    auto context = engine.run_after_preflight(request, *lease);
    if (!context) {
        return std::unexpected(context.error());
    }
    return LaunchOutcome{std::move(resolved), std::move(*context)};
}

}  // namespace hoyoflux::app
