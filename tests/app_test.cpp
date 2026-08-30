#include "app/launch_service.hpp"
#include "profile/config.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace hoyoflux;

TEST_CASE("native argument count selects one-click or CLI", "[app][b1-8]") {
    CHECK(app::select_invocation_mode(1) == app::InvocationMode::OneClick);
    CHECK(app::select_invocation_mode(2) == app::InvocationMode::Cli);
    CHECK(app::select_invocation_mode(5) == app::InvocationMode::Cli);
}

TEST_CASE("shared launch resolver preserves explicit profile overrides",
          "[app][b1-8]") {
    auto config = profile::parse_config(profile::default_config_toml());
    REQUIRE(config.has_value());
    app::LaunchOptions options;
    options.game = GameId::Genshin;
    options.profile = "desktop";
    options.fps_override = 144;
    options.mobile_ui_override = true;
    options.dpi_override = 1.5f;
    auto resolved = app::resolve_launch(*config, options);
    REQUIRE(resolved.has_value());
    CHECK(resolved->profile.id == "desktop");
    CHECK(resolved->profile.runtime.fps == 144);
    CHECK(resolved->profile.ui.mobile_ui);
    REQUIRE(resolved->profile.ui.dpi_scale.has_value());
    CHECK(*resolved->profile.ui.dpi_scale == 1.5f);
    CHECK_FALSE(resolved->auto_decision.has_value());
}
