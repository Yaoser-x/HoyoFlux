#include "profile/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

using namespace hoyoflux;
namespace profile = hoyoflux::profile;

namespace {

class TempDirectory {
public:
    explicit TempDirectory(std::string_view label) {
        path_ = std::filesystem::temp_directory_path() /
            ("hoyoflux-profile-" + std::string(label) + "-" +
             std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(file.good());
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

win32::DisplayInfo display(uint32_t index, int width, int height) {
    win32::DisplayInfo result;
    result.index = index;
    result.device_name = L"\\\\.\\DISPLAY_TEST_" + std::to_wstring(index);
    result.is_attached = true;
    result.left = 0;
    result.top = 0;
    result.right = width;
    result.bottom = height;
    return result;
}

}  // namespace

TEST_CASE("default portable config is concise schema 2", "[profile][config]") {
    const std::string document = profile::default_config_toml();
    CHECK(document.find("schema = 2") != std::string::npos);
    CHECK(document.find("preset_revision") == std::string::npos);
    CHECK(document.find("[profiles.desktop.runtime]") == std::string::npos);
    CHECK(document.find("example_mobile") != std::string::npos);

    auto parsed = profile::parse_config(document);
    REQUIRE(parsed.has_value());
    CHECK(parsed->schema == 2);
    CHECK(parsed->launcher.action == profile::LauncherAction::Launch);
    CHECK(parsed->launcher.profile == "auto");
    CHECK(parsed->genshin_default == "desktop");
    CHECK(parsed->starrail_default == "starrail_desktop");
    REQUIRE(parsed->profiles.size() == 2);
    CHECK(profile::find_profile(*parsed, "desktop")->runtime.fps == 120);
    CHECK(profile::find_profile(*parsed, "starrail_desktop")->game ==
          GameId::StarRail);
}

TEST_CASE("flat profile maps every supported setting", "[profile][schema2]") {
    TempDirectory temp("flat");
    auto parsed = profile::parse_config(R"(
schema = 2
[launcher]
game = "genshin"
profile = "mobile"
action = "diagnose"
region = "global"
notifications = false
[defaults]
genshin = "mobile"
starrail = "rail"
[profiles.mobile]
game = "genshin"
fps = 144
resolution = "2560x1440"
fullscreen = "windowed"
persistence = "persistent"
monitor = 2
priority = "high"
power_save = true
power_save_fps = 24
hotkeys = true
mobile_ui = true
dpi_scale = 1.5
exe = "游戏/Genshin.exe"
args = ["-popupwindow", "中文参数"]
match = { resolution = "1920x1080", priority = 50 }
[profiles.rail]
game = "starrail"
fps = 90
)", temp.path());
    REQUIRE(parsed.has_value());
    CHECK(parsed->launcher.action == profile::LauncherAction::Diagnose);
    CHECK(parsed->launcher.region == profile::LauncherRegion::Global);
    CHECK_FALSE(parsed->launcher.notifications);
    auto mobile = profile::find_profile(*parsed, "mobile");
    REQUIRE(mobile.has_value());
    CHECK(mobile->runtime.fps == 144);
    CHECK(mobile->render.resolution == std::optional<Resolution>{{2560, 1440}});
    CHECK(mobile->render.persistence == ResolutionPersistence::Persistent);
    CHECK(mobile->runtime.priority == ProcessPriority::High);
    CHECK(mobile->runtime.power_save == PowerSavePolicy::Enabled);
    CHECK(mobile->runtime.power_save_fps == 24);
    CHECK(mobile->runtime.hotkeys);
    CHECK(mobile->ui.mobile_ui);
    CHECK(mobile->ui.dpi_scale == std::optional<float>{1.5f});
    REQUIRE(mobile->executable.has_value());
    CHECK(*mobile->executable == temp.path() / L"游戏/Genshin.exe");
    REQUIRE(mobile->arguments.size() == 2);
    CHECK(mobile->arguments[1] == L"中文参数");
    CHECK(mobile->match.auto_select);
    CHECK(mobile->match.priority == 50);
}

TEST_CASE("match table enables auto selection unless explicitly disabled",
          "[profile][auto]") {
    auto parsed = profile::parse_config(R"(
schema = 2
[launcher]
[profiles.auto]
game = "genshin"
match = { portrait = true }
[profiles.manual]
game = "genshin"
match = { auto_select = false, resolution = "1920x1080" }
)");
    REQUIRE(parsed.has_value());
    CHECK(profile::find_profile(*parsed, "auto")->match.auto_select);
    CHECK_FALSE(profile::find_profile(*parsed, "manual")->match.auto_select);
}

TEST_CASE("schema 2 rejects unknown and mistyped fields with locations",
          "[profile][validation]") {
    auto unknown = profile::parse_config(R"(
schema = 2
[launcher]
[profiles.desktop]
game = "genshin"
fpps = 120
)");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().message.find("unknown key 'fpps'") != std::string::npos);
    CHECK(unknown.error().message.find("line") != std::string::npos);

    auto wrong_type = profile::parse_config(R"(
schema = 2
[launcher]
[profiles.desktop]
game = "genshin"
fps = "fast"
)");
    REQUIRE_FALSE(wrong_type.has_value());
    CHECK(wrong_type.error().message.find("fps must be an integer") !=
          std::string::npos);
    CHECK(wrong_type.error().message.find("line") != std::string::npos);

    auto bad_range = profile::parse_config(R"(
schema = 2
[launcher]
[profiles.desktop]
game = "genshin"
fps = 5001
    )");
    REQUIRE_FALSE(bad_range.has_value());
    CHECK(bad_range.error().message.find("profile 'desktop'.fps") !=
          std::string::npos);
    CHECK(bad_range.error().message.find("line") != std::string::npos);
}

TEST_CASE("valid diagnose action remains readable when a profile is invalid",
          "[profile][validation]") {
    TempDirectory temp("diagnose-invalid-profile");
    const auto path = temp.path() / "config.toml";
    write_file(path, R"(schema = 2
[launcher]
action = "diagnose"
[profiles.ipad]
game = "genshin"
dpi_scale = 9.0
)" );
    auto config = profile::read_config(path);
    REQUIRE_FALSE(config.has_value());
    REQUIRE(config.error().location.has_value());
    CHECK(config.error().location->profile == "ipad");
    CHECK(config.error().location->field == "dpi_scale");
    CHECK(config.error().location->line.has_value());
    auto action = profile::read_declared_launcher_action(path);
    REQUIRE(action.has_value());
    CHECK(*action == profile::LauncherAction::Diagnose);
}

TEST_CASE("schema 1 loads once into schema 2 with an exact backup",
          "[profile][migration]") {
    TempDirectory temp("migration");
    const auto config_path = temp.path() / "config.toml";
    const auto backup_path = temp.path() / "data" / "backups";
    const std::string legacy = R"(schema = 1
preset_revision = 4
[launcher]
game = "genshin"
profile = "auto"
region = "cn"
notifications = false
[defaults]
genshin = "custom"
starrail = "rail"
[profiles.custom]
game = "genshin"
[profiles.custom.match]
auto_select = true
resolution = "1920x1080"
priority = 9
[profiles.custom.render]
resolution = "2560x1440"
persistence = "session"
[profiles.custom.runtime]
fps = 165
hotkeys = true
[profiles.custom.runtime.power_save]
enabled = true
fps = 25
[profiles.custom.ui]
mobile_ui = true
dpi_scale = 1.25
[profiles.rail]
game = "starrail"
[profiles.rail.runtime]
fps = 90
)";
    write_file(config_path, legacy);

    const std::string before_prepare = read_file(config_path);
    auto inspected = profile::read_config(config_path);
    REQUIRE(inspected.has_value());
    CHECK(inspected->schema == 1);
    CHECK(read_file(config_path) == before_prepare);
    CHECK_FALSE(std::filesystem::exists(backup_path));

    auto prepared = profile::prepare_config_migration(config_path);
    REQUIRE(prepared.has_value());
    CHECK(read_file(config_path) == before_prepare);
    auto migrated = profile::commit_config_migration(config_path, backup_path, *prepared);
    REQUIRE(migrated.has_value());
    CHECK(migrated->schema == 2);
    CHECK(migrated->launcher.region == profile::LauncherRegion::Cn);
    CHECK_FALSE(migrated->launcher.notifications);
    auto custom = profile::find_profile(*migrated, "custom");
    REQUIRE(custom.has_value());
    CHECK(custom->runtime.fps == 165);
    CHECK(custom->runtime.hotkeys);
    CHECK(custom->runtime.power_save_fps == 25);
    CHECK(custom->ui.mobile_ui);
    CHECK(custom->match.priority == 9);
    CHECK(read_file(backup_path / "config.schema1.toml") == legacy);
    const std::string current = read_file(config_path);
    CHECK(current.find("schema = 2") != std::string::npos);
    CHECK(current.find("preset_revision") == std::string::npos);

    auto second = profile::read_config(config_path);
    REQUIRE(second.has_value());
    CHECK(read_file(config_path) == current);
}

TEST_CASE("default materialization is explicit and never overwrites",
          "[profile][first-run]") {
    TempDirectory temp("first-run");
    const auto path = temp.path() / "config.toml";
    auto missing = profile::read_config(path);
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(profile::write_default_config(path).has_value());
    const std::string original = read_file(path);
    REQUIRE(profile::write_default_config(path).has_value());
    CHECK(read_file(path) == original);
    REQUIRE(profile::read_config(path).has_value());
}

TEST_CASE("schema migration keeps high precision DPI and never changes an inspection",
          "[profile][migration]") {
    TempDirectory temp("precision");
    const auto path = temp.path() / "config.toml";
    const auto backups = temp.path() / "backups";
    const std::string legacy = R"(schema = 1
preset_revision = 4
[launcher]
game = "genshin"
profile = "ipad"
[profiles.ipad]
game = "genshin"
[profiles.ipad.runtime]
fps = 60
[profiles.ipad.ui]
mobile_ui = true
dpi_scale = 2.1234567
)";
    write_file(path, legacy);

    REQUIRE(profile::read_config(path).has_value());
    CHECK(read_file(path) == legacy);
    auto prepared = profile::prepare_config_migration(path);
    REQUIRE(prepared.has_value());
    REQUIRE(profile::configs_equivalent(prepared->source, prepared->converted));
    auto migrated = profile::commit_config_migration(path, backups, *prepared);
    REQUIRE(migrated.has_value());
    const auto ipad = profile::find_profile(*migrated, "ipad");
    REQUIRE(ipad.has_value());
    CHECK(ipad->ui.dpi_scale == std::optional<float>{2.1234567f});
}

TEST_CASE("schema 2 serializer round trips quoted profile ids",
          "[profile][serialization]") {
    auto config = profile::parse_config(profile::default_config_toml());
    REQUIRE(config.has_value());
    config->profiles.front().id = "desk top";
    config->genshin_default = "desk top";
    const auto text = profile::serialize_config(*config);
    auto reparsed = profile::parse_config(text);
    REQUIRE(reparsed.has_value());
    CHECK(profile::find_profile(*reparsed, "desk top").has_value());
    CHECK(reparsed->genshin_default == "desk top");
}

TEST_CASE("automatic matching keeps specificity priority and fallback rules",
          "[profile][auto]") {
    profile::Config config;
    config.genshin_default = "desktop";
    Profile fallback;
    fallback.id = "desktop";
    fallback.game = GameId::Genshin;
    Profile portrait;
    portrait.id = "portrait";
    portrait.game = GameId::Genshin;
    portrait.match.auto_select = true;
    portrait.match.portrait = true;
    portrait.match.priority = 1;
    Profile exact = portrait;
    exact.id = "exact";
    exact.match.resolution = Resolution{1080, 1920};
    config.profiles = {fallback, portrait, exact};

    auto decision = profile::resolve_auto_profile(
        config, GameId::Genshin, {display(1, 1080, 1920)});
    REQUIRE(decision.has_value());
    CHECK(decision->profile.id == "exact");
    CHECK_FALSE(decision->used_fallback);

    auto fallback_decision = profile::resolve_auto_profile(
        config, GameId::Genshin, {display(1, 1920, 1080)});
    REQUIRE(fallback_decision.has_value());
    CHECK(fallback_decision->profile.id == "desktop");
    CHECK(fallback_decision->used_fallback);
}

TEST_CASE("equal auto rank remains an explicit ambiguity",
          "[profile][auto]") {
    profile::Config config;
    Profile first;
    first.id = "first";
    first.game = GameId::Genshin;
    first.match.auto_select = true;
    first.match.portrait = true;
    Profile second = first;
    second.id = "second";
    config.profiles = {first, second};
    auto result = profile::resolve_auto_profile(
        config, GameId::Genshin, {display(1, 1080, 1920)});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::AutoProfileAmbiguous);
}
