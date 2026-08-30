// Profile store tests: parsing, defaults, validation, matching.

#include "profile/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <string>

using namespace hoyoflux;
namespace profile = hoyoflux::profile;

TEST_CASE("default config parses into the built-in presets",
          "[profile][config]") {
    auto config = profile::parse_config(profile::default_config_toml());
    REQUIRE(config.has_value());
    REQUIRE(config->profiles.size() == 4);
    CHECK(config->genshin_default == "desktop");
    CHECK(config->starrail_default == "starrail_desktop");

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
    CHECK(ipad->render.resolution->width == 2266);
    CHECK(ipad->render.resolution->height == 1488);
    CHECK(ipad->runtime.fps == 60);
    REQUIRE(ipad->match.resolution.has_value());
    CHECK(ipad->match.resolution->width == 2266);
    CHECK(ipad->match.resolution->height == 1488);
    CHECK_FALSE(ipad->match.portrait.has_value());
    CHECK(ipad->match.auto_select);  // legacy "match = auto" string form

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
    CHECK(profile.match.auto_select);
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

TEST_CASE("auto matching picks iPad mini profile for its exact mode",
          "[profile][matcher]") {
    auto config = profile::parse_config(profile::default_config_toml());
    REQUIRE(config.has_value());

    win32::DisplayInfo landscape;
    landscape.is_attached = true;
    landscape.left = 0;
    landscape.top = 0;
    landscape.right = 2560;
    landscape.bottom = 1440;

    win32::DisplayInfo ipad_mini;
    ipad_mini.is_attached = true;
    ipad_mini.left = 2560;
    ipad_mini.top = 0;
    ipad_mini.right = 4826;
    ipad_mini.bottom = 1488;  // 2266x1488 landscape mode

    auto desktop = profile::match_auto_profile(*config, GameId::Genshin, {landscape});
    REQUIRE(desktop.has_value());
    CHECK(desktop->id == "desktop");

    auto mobile = profile::match_auto_profile(
        *config, GameId::Genshin, {landscape, ipad_mini});
    REQUIRE(mobile.has_value());
    CHECK(mobile->id == "ipad");
    CHECK(mobile->ui.mobile_ui);

    // Star Rail has its own deterministic fallback.
    auto starrail =
        profile::match_auto_profile(*config, GameId::StarRail, {ipad_mini});
    REQUIRE(starrail.has_value());
    CHECK(starrail->id == "starrail_desktop");
}

TEST_CASE("auto ranking keeps specificity separate from priority",
          "[profile][matcher][b1-8]") {
    auto config = profile::parse_config(R"(
[defaults]
genshin = "desktop"
[profiles.desktop]
game = "genshin"
[profiles.orientation]
game = "genshin"
[profiles.orientation.match]
auto_select = true
portrait = false
priority = 1000
[profiles.exact]
game = "genshin"
[profiles.exact.match]
auto_select = true
resolution = "2560x1440"
priority = 0
)");
    REQUIRE(config.has_value());
    win32::DisplayInfo display;
    display.index = 7;
    display.is_attached = true;
    display.right = 2560;
    display.bottom = 1440;
    auto decision = profile::resolve_auto_profile(*config, GameId::Genshin,
                                                  {display});
    REQUIRE(decision.has_value());
    CHECK(decision->profile.id == "exact");
    CHECK(decision->specificity == 100);
    CHECK(decision->priority == 0);
    REQUIRE(decision->display_index.has_value());
    CHECK(*decision->display_index == 7);
}

TEST_CASE("auto priority breaks equal specificity and equal rank is ambiguous",
          "[profile][matcher][b1-8]") {
    const auto make_config = [](int second_priority) {
        return profile::parse_config(
            "[profiles.first]\ngame=\"genshin\"\n"
            "[profiles.first.match]\nauto_select=true\nportrait=false\npriority=10\n"
            "[profiles.second]\ngame=\"genshin\"\n"
            "[profiles.second.match]\nauto_select=true\nportrait=false\npriority=" +
            std::to_string(second_priority) + "\n");
    };
    win32::DisplayInfo display;
    display.is_attached = true;
    display.right = 1920;
    display.bottom = 1080;
    auto ranked_config = make_config(20);
    REQUIRE(ranked_config.has_value());
    auto ranked = profile::resolve_auto_profile(*ranked_config, GameId::Genshin,
                                                {display});
    REQUIRE(ranked.has_value());
    CHECK(ranked->profile.id == "second");
    auto tied_config = make_config(10);
    REQUIRE(tied_config.has_value());
    auto tied = profile::resolve_auto_profile(*tied_config, GameId::Genshin,
                                              {display});
    REQUIRE_FALSE(tied.has_value());
    CHECK(tied.error().code == ErrorCode::AutoProfileAmbiguous);
    CHECK(tied.error().message.find("first") != std::string::npos);
    CHECK(tied.error().message.find("second") != std::string::npos);
}

TEST_CASE("per-game defaults precede the legacy fallback",
          "[profile][matcher][b1-8]") {
    auto config = profile::parse_config(R"(
default_profile = "legacy"
[defaults]
genshin = "genshin_desktop"
starrail = "starrail_desktop"
[profiles.legacy]
game = "genshin"
[profiles.genshin_desktop]
game = "genshin"
[profiles.starrail_desktop]
game = "starrail"
)");
    REQUIRE(config.has_value());
    auto genshin = profile::resolve_auto_profile(*config, GameId::Genshin, {});
    REQUIRE(genshin.has_value());
    CHECK(genshin->profile.id == "genshin_desktop");
    CHECK(genshin->used_fallback);
    auto starrail = profile::resolve_auto_profile(*config, GameId::StarRail, {});
    REQUIRE(starrail.has_value());
    CHECK(starrail->profile.id == "starrail_desktop");
    auto legacy = profile::parse_config(R"(
default_profile = "legacy"
[profiles.legacy]
game = "genshin"
)");
    REQUIRE(legacy.has_value());
    auto legacy_result = profile::resolve_auto_profile(*legacy, GameId::Genshin, {});
    REQUIRE(legacy_result.has_value());
    CHECK(legacy_result->profile.id == "legacy");
}

TEST_CASE("detached displays do not participate in auto matching",
          "[profile][matcher][b1-8]") {
    auto config = profile::parse_config(profile::default_config_toml());
    REQUIRE(config.has_value());
    win32::DisplayInfo detached;
    detached.is_attached = false;
    detached.right = 2266;
    detached.bottom = 1488;
    auto decision = profile::resolve_auto_profile(*config, GameId::Genshin,
                                                  {detached});
    REQUIRE(decision.has_value());
    CHECK(decision->profile.id == "desktop");
    CHECK(decision->used_fallback);
    CHECK(decision->displays.empty());
}

TEST_CASE("auto matching: identity beats geometry, manual never auto-picked",
          "[profile][matcher][f8]") {
    // TOML exercising the structured [match] tables and the tiers.
    const char* toml = R"(
default_profile = "desktop"

[profiles.desktop]
game = "genshin"

[profiles.portrait_profile]
game = "genshin"
[profiles.portrait_profile.match]
auto_select = true
portrait = true

[profiles.exact_profile]
game = "genshin"
[profiles.exact_profile.match]
auto_select = true
resolution = "2560x1440"

[profiles.manual_profile]
game = "genshin"
mobile_ui = true
[profiles.manual_profile.match]
auto_select = false
portrait = true
)";
    auto config = profile::parse_config(toml);
    REQUIRE(config.has_value());

    win32::DisplayInfo landscape_1440;
    landscape_1440.is_attached = true;
    // Fake device name: the mode query fails and the matcher falls back to
    // the geometry below (2560x1440).
    landscape_1440.device_name = L"\\\\.\\HOYOFLUX_TEST_LANDSCAPE";
    landscape_1440.right = 2560;
    landscape_1440.bottom = 1440;

    // Portrait-only candidate loses to the exact-resolution candidate
    // (geometry tier below identity of resolution).
    auto picked = profile::match_auto_profile(*config, GameId::Genshin,
                                              {landscape_1440});
    REQUIRE(picked.has_value());
    CHECK(picked->id == "exact_profile");

    // The manual profile (portrait match) is NEVER auto-selected, even
    // though it matches the display and would otherwise be a candidate.
    win32::DisplayInfo portrait_display;
    portrait_display.is_attached = true;
    portrait_display.device_name = L"\\\\.\\HOYOFLUX_TEST_PORTRAIT";
    portrait_display.right = 1080;
    portrait_display.bottom = 1920;
    auto not_manual = profile::match_auto_profile(
        *config, GameId::Genshin, {portrait_display});
    REQUIRE(not_manual.has_value());
    CHECK(not_manual->id == "portrait_profile");
    CHECK(not_manual->id != "manual_profile");
}

TEST_CASE("config parsing never throws on malformed values (F9)",
          "[profile][f9]") {
    // Every one of these used to escape std::invalid_argument.
    const char* bad_docs[] = {
        "[profiles.p]\ngame = \"genshin\"\n[profiles.p.render]\nresolution = \"abcxdef\"\n",
        "[profiles.p]\ngame = \"genshin\"\n[profiles.p.render]\nresolution = \"-5x30\"\n",
        "[profiles.p]\ngame = \"genshin\"\n[profiles.p.render]\nresolution = \"20x\"\n",
        "[profiles.p]\ngame = \"genshin\"\n[profiles.p.render]\nresolution = \" 20x30\"\n",
        "[profiles.p]\ngame = \"genshin\"\n[profiles.p.render]\nmonitor = -1\n",
        "[profiles.p]\ngame = \"genshin\"\n[profiles.p.render]\nmonitor = 999\n",
        "[profiles.p]\ngame = \"genshin\"\n[profiles.p.runtime.power_save]\nfps = 0\n",
        "schema = 2\n[profiles.p]\ngame = \"genshin\"\n",
    };
    for (const char* doc : bad_docs) {
        INFO(doc);
        auto parsed = profile::parse_config(doc);
        REQUIRE_FALSE(parsed.has_value());
        CHECK((parsed.error().code == ErrorCode::ProfileInvalid ||
               parsed.error().code == ErrorCode::ConfigParseFailed));
    }

    // Monitor 63 is fine, schema 1 is accepted.
    auto ok = profile::parse_config(
        "[profiles.p]\ngame = \"genshin\"\nschema = 1\n"
        "[profiles.p.render]\nmonitor = 63\n");
    REQUIRE(ok.has_value());
    CHECK(ok->profiles.size() == 1);
}

TEST_CASE("first run materializes config.toml (F9)", "[profile][f9]") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("hoyoflux_f9_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(dir);
    const auto path = dir / "config.toml";

    auto config = profile::load_config(path);
    REQUIRE(config.has_value());
    CHECK_FALSE(config->profiles.empty());

    // The file now exists on disk with the same content shape.
    REQUIRE(std::filesystem::exists(path));
    auto reloaded = profile::load_config(path);
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->profiles.size() == config->profiles.size());
    CHECK(reloaded->default_profile == config->default_profile);

    std::filesystem::remove_all(dir);
}
