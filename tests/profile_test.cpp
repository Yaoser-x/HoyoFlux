// Profile store tests: parsing, defaults, validation, matching.

#include "profile/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace hoyoflux;
namespace profile = hoyoflux::profile;

TEST_CASE("default config parses into the built-in presets",
          "[profile][config]") {
    auto config = profile::parse_config(profile::default_config_toml());
    REQUIRE(config.has_value());
    REQUIRE(config->profiles.size() == 4);

    auto desktop = profile::find_profile(*config, "desktop");
    REQUIRE(desktop.has_value());
    CHECK(desktop->game == GameId::Genshin);
    CHECK(desktop->runtime.fps == 120);
    CHECK_FALSE(desktop->ui.mobile_ui);
    CHECK(desktop->render.persistence == ResolutionPersistence::Session);
    CHECK_FALSE(desktop->ui.dpi_scale.has_value());

    auto ipad = profile::find_profile(*config, "ipad");
    REQUIRE(ipad.has_value());
    CHECK(ipad->ui.mobile_ui);
    REQUIRE(ipad->ui.dpi_scale.has_value());
    CHECK(*ipad->ui.dpi_scale == 2.0f);
    REQUIRE(ipad->render.resolution.has_value());
    CHECK(ipad->render.resolution->width == 1080);
    CHECK(ipad->render.resolution->height == 1920);
    CHECK(ipad->match == MatchPolicy::Auto);

    auto starrail = profile::find_profile(*config, "starrail_desktop");
    REQUIRE(starrail.has_value());
    CHECK(starrail->game == GameId::StarRail);
}

TEST_CASE("every field maps into the typed profile", "[profile][config]") {
    const std::string doc = R"(
default_profile = "pro"

[profiles.pro]
game = "starrail"
match = "auto"

[profiles.pro.render]
resolution = "3440x1440"
fullscreen = "exclusive"
persistence = "persistent"
monitor = 1

[profiles.pro.runtime]
fps = 240
priority = "high"

[profiles.pro.runtime.power_save]
enabled = true
fps = 45

[profiles.pro.ui]
mobile_ui = false
dpi_scale = 1.25
)";
    auto config = profile::parse_config(doc);
    REQUIRE(config.has_value());
    CHECK(config->default_profile == "pro");
    auto found = profile::find_profile(*config, "pro");
    REQUIRE(found.has_value());
    const Profile& profile = *found;
    CHECK(profile.game == GameId::StarRail);
    CHECK(profile.match == MatchPolicy::Auto);
    REQUIRE(profile.render.resolution.has_value());
    CHECK(profile.render.resolution->width == 3440);
    CHECK(profile.render.fullscreen == FullscreenMode::Exclusive);
    CHECK(profile.render.persistence == ResolutionPersistence::Persistent);
    REQUIRE(profile.render.monitor.has_value());
    CHECK(*profile.render.monitor == 1);
    CHECK(profile.runtime.fps == 240);
    CHECK(profile.runtime.priority == ProcessPriority::High);
    CHECK(profile.runtime.power_save == PowerSavePolicy::Enabled);
    CHECK(profile.runtime.power_save_fps == 45);
    REQUIRE(profile.ui.dpi_scale.has_value());
    CHECK(*profile.ui.dpi_scale == 1.25f);
}

TEST_CASE("unknown keys are ignored, malformed values are reported",
          "[profile][config]") {
    const std::string doc = R"(
[profiles.ok]
game = "genshin"
future_field = "whatever"

[profiles.ok.runtime]
fps = 60
unknown_key = true
)";
    auto config = profile::parse_config(doc);
    REQUIRE(config.has_value());
    CHECK(config->profiles.size() == 1);

    auto bad_fps = profile::parse_config(R"(
[profiles.bad]
game = "genshin"
[profiles.bad.runtime]
fps = 5000
)");
    REQUIRE_FALSE(bad_fps.has_value());
    CHECK(bad_fps.error().code == ErrorCode::ProfileInvalid);

    auto bad_game = profile::parse_config(R"(
[profiles.bad]
game = "wuthering_waves"
)");
    REQUIRE_FALSE(bad_game.has_value());
    CHECK(bad_game.error().code == ErrorCode::ProfileInvalid);

    auto bad_toml = profile::parse_config("[profiles.bad\ngame =");
    REQUIRE_FALSE(bad_toml.has_value());
    CHECK(bad_toml.error().code == ErrorCode::ConfigParseFailed);

    auto missing_game = profile::parse_config("[profiles.bad]\nfps = 60");
    REQUIRE_FALSE(missing_game.has_value());
    CHECK(missing_game.error().code == ErrorCode::ProfileInvalid);
}

TEST_CASE("missing profile id reports not found", "[profile][config]") {
    auto config = profile::parse_config(profile::default_config_toml());
    REQUIRE(config.has_value());
    auto missing = profile::find_profile(*config, "nonexistent");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::ProfileNotFound);
}

TEST_CASE("auto matching picks mobile profiles for portrait displays",
          "[profile][matcher]") {
    auto config = profile::parse_config(profile::default_config_toml());
    REQUIRE(config.has_value());

    win32::DisplayInfo landscape;
    landscape.is_attached = true;
    landscape.left = 0;
    landscape.top = 0;
    landscape.right = 2560;
    landscape.bottom = 1440;

    win32::DisplayInfo portrait;
    portrait.is_attached = true;
    portrait.left = 2560;
    portrait.top = 0;
    portrait.right = 3640;
    portrait.bottom = 2560;  // 1080x2560 -> portrait

    auto desktop = profile::match_auto_profile(*config, GameId::Genshin, {landscape});
    REQUIRE(desktop.has_value());
    CHECK(desktop->id == "desktop");
    CHECK_FALSE(desktop->ui.mobile_ui);

    auto mobile = profile::match_auto_profile(*config, GameId::Genshin, {landscape, portrait});
    REQUIRE(mobile.has_value());
    CHECK(mobile->id == "ipad");
    CHECK(mobile->ui.mobile_ui);

    // No starrail mobile profile exists -> error, not a silent wrong pick.
    auto starrail = profile::match_auto_profile(*config, GameId::StarRail, {portrait});
    REQUIRE_FALSE(starrail.has_value());
    CHECK(starrail.error().code == ErrorCode::ProfileNotFound);
}
