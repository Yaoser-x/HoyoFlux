#include "app/launch_service.hpp"
#include "app/doctor.hpp"
#include "app/portable_migration.hpp"
#include "platform/win32/file.hpp"
#include "profile/config.hpp"
#include "session/journal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace hoyoflux;

namespace {

class TempTree {
public:
    explicit TempTree(std::string_view name) {
        root_ = std::filesystem::temp_directory_path() /
            ("hoyoflux-app-" + std::string(name) + "-" +
             std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_);
    }
    ~TempTree() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    const std::filesystem::path& root() const { return root_; }
private:
    std::filesystem::path root_;
};

app::AppPaths paths_for(const std::filesystem::path& root) {
    app::AppPaths paths;
    paths.executable = root / "hoyoflux.exe";
    paths.root = root;
    paths.config = root / "config.toml";
    paths.data = root / "data";
    paths.state = paths.data / "state";
    paths.journal = paths.state / "active-session.json";
    paths.migration_record = paths.state / "portable-migration.toml";
    paths.backups = paths.data / "backups";
    paths.diagnostics = paths.data / "diagnostics.txt";
    std::filesystem::create_directories(paths.state);
    std::filesystem::create_directories(paths.backups);
    return paths;
}

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::filesystem::path find_backup(const app::AppPaths& paths,
                                  std::wstring_view prefix) {
    for (const auto& entry : std::filesystem::directory_iterator(paths.backups)) {
        if (entry.is_regular_file() &&
            entry.path().filename().wstring().starts_with(prefix)) {
            return entry.path();
        }
    }
    return {};
}

session::ActiveSessionJournal journal(std::string id) {
    session::ActiveSessionJournal result;
    result.session_id = std::move(id);
    result.game = GameId::Genshin;
    result.stage = SessionStage::Preparing;
    return result;
}

}  // namespace

TEST_CASE("portable paths are derived from the executable and writable locally",
          "[app][paths]") {
    auto actual = app::resolve_app_paths();
    REQUIRE(actual.has_value());
    CHECK(actual->root == actual->executable.parent_path());
    CHECK(actual->config == actual->root / "config.toml");
    CHECK(actual->journal == actual->root / "data" / "state" /
          "active-session.json");

    TempTree tree("paths");
    auto isolated = paths_for(tree.root() / "portable");
    REQUIRE(app::prepare_portable_directories(isolated).has_value());
    CHECK(std::filesystem::is_directory(isolated.state));
    CHECK(std::filesystem::is_directory(isolated.backups));
}

TEST_CASE("launch resolution consumes only configuration", "[app][config]") {
    auto config = profile::parse_config(R"(
schema = 2
[launcher]
profile = "desktop"
[profiles.desktop]
game = "genshin"
fps = 144
mobile_ui = true
dpi_scale = 1.5
exe = "game/Genshin.exe"
args = ["-popupwindow"]
)", L"C:/Portable/HoyoFlux");
    REQUIRE(config.has_value());
    app::LaunchOptions options;
    options.game = GameId::Genshin;
    options.profile = "desktop";
    options.journal_path = L"C:/Portable/HoyoFlux/data/state/active-session.json";
    auto resolved = app::resolve_launch(*config, options);
    REQUIRE(resolved.has_value());
    CHECK(resolved->profile.runtime.fps == 144);
    CHECK(resolved->profile.ui.mobile_ui);
    CHECK(resolved->profile.executable ==
          std::optional<std::filesystem::path>{L"C:/Portable/HoyoFlux/game/Genshin.exe"});
    CHECK(resolved->profile.arguments == std::vector<std::wstring>{L"-popupwindow"});
}

TEST_CASE("AppData config and journal migrate with exact local backups",
          "[app][migration]") {
    TempTree tree("import");
    const auto paths = paths_for(tree.root() / "portable");
    const auto legacy = tree.root() / "appdata" / "HoyoFlux";
    const std::string legacy_config = R"(schema = 1
preset_revision = 4
[launcher]
game = "genshin"
profile = "desktop"
[profiles.desktop]
game = "genshin"
[profiles.desktop.runtime]
fps = 165
)";
    write_file(legacy / "config.toml", legacy_config);
    write_file(legacy / "keep.me", "unknown");
    REQUIRE(session::save_journal(
        legacy / "state" / "active-session.json", journal("legacy")));

    auto result = app::migrate_legacy_data(paths, legacy);
    REQUIRE(result.has_value());
    CHECK(result->imported_config);
    CHECK(result->imported_journal);
    CHECK(result->removed_legacy_config);
    REQUIRE(std::filesystem::exists(paths.config));
    CHECK(read_file(paths.config).find("schema = 2") != std::string::npos);
    const auto config_backup = find_backup(paths, L"appdata-config-");
    REQUIRE_FALSE(config_backup.empty());
    CHECK(read_file(config_backup) == legacy_config);
    REQUIRE(session::load_journal(paths.journal)->has_value());
    CHECK_FALSE(std::filesystem::exists(legacy / "config.toml"));
    CHECK_FALSE(std::filesystem::exists(legacy / "state" / "active-session.json"));
    CHECK_FALSE(std::filesystem::exists(paths.migration_record));
    CHECK(std::filesystem::exists(legacy / "keep.me"));
}

TEST_CASE("existing portable config wins while AppData config is archived",
          "[app][migration]") {
    TempTree tree("local-wins");
    const auto paths = paths_for(tree.root() / "portable");
    const auto legacy = tree.root() / "appdata" / "HoyoFlux";
    REQUIRE(profile::write_default_config(paths.config));
    const std::string local = read_file(paths.config);
    const std::string old = R"(schema = 2
[launcher]
[profiles.old]
game = "genshin"
fps = 60
)";
    write_file(legacy / "config.toml", old);

    auto result = app::migrate_legacy_data(paths, legacy);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->imported_config);
    CHECK(read_file(paths.config) == local);
    const auto config_backup = find_backup(paths, L"appdata-config-");
    REQUIRE_FALSE(config_backup.empty());
    CHECK(read_file(config_backup) == old);
    CHECK_FALSE(std::filesystem::exists(legacy / "config.toml"));
}

TEST_CASE("different local and AppData journals stop migration without deletion",
          "[app][migration][journal]") {
    TempTree tree("journal-conflict");
    const auto paths = paths_for(tree.root() / "portable");
    const auto legacy = tree.root() / "appdata" / "HoyoFlux";
    REQUIRE(session::save_journal(paths.journal, journal("local")));
    const auto legacy_journal = legacy / "state" / "active-session.json";
    REQUIRE(session::save_journal(legacy_journal, journal("legacy")));

    auto result = app::migrate_legacy_data(paths, legacy);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::JournalCorrupt);
    CHECK(std::filesystem::exists(paths.journal));
    CHECK(std::filesystem::exists(legacy_journal));
}

TEST_CASE("corrupt AppData journal remains at its source", "[app][migration][journal]") {
    TempTree tree("journal-corrupt");
    const auto paths = paths_for(tree.root() / "portable");
    const auto legacy = tree.root() / "appdata" / "HoyoFlux";
    const auto legacy_journal = legacy / "state" / "active-session.json";
    write_file(legacy_journal, "{broken");

    auto result = app::migrate_legacy_data(paths, legacy);
    REQUIRE_FALSE(result.has_value());
    CHECK(std::filesystem::exists(legacy_journal));
    CHECK_FALSE(std::filesystem::exists(paths.journal));
}

TEST_CASE("invalid imported config rolls back the local copy for a safe retry",
          "[app][migration][retry]") {
    TempTree tree("invalid-config");
    const auto paths = paths_for(tree.root() / "portable");
    const auto legacy = tree.root() / "appdata" / "HoyoFlux";
    const auto old_config = legacy / "config.toml";
    write_file(old_config, "schema = 2\n[profiles.broken\n");

    auto result = app::migrate_legacy_data(paths, legacy);
    REQUIRE_FALSE(result.has_value());
    CHECK(std::filesystem::exists(old_config));
    CHECK_FALSE(find_backup(paths, L"appdata-config-").empty());
    CHECK_FALSE(std::filesystem::exists(paths.config));
}

TEST_CASE("a running legacy session blocks migration and preserves its journal",
          "[app][migration][journal]") {
    TempTree tree("journal-running");
    const auto paths = paths_for(tree.root() / "portable");
    const auto legacy = tree.root() / "appdata" / "HoyoFlux";
    const auto legacy_journal = legacy / "state" / "active-session.json";
    auto active = journal("running");
    active.pid = GetCurrentProcessId();
    REQUIRE(session::save_journal(legacy_journal, active));

    auto result = app::migrate_legacy_data(paths, legacy);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::SessionAlreadyActive);
    CHECK(std::filesystem::exists(legacy_journal));
    CHECK_FALSE(std::filesystem::exists(paths.journal));
}

TEST_CASE("an interrupted prepared migration resumes only for matching files",
          "[app][migration][resume]") {
    TempTree tree("resume");
    const auto paths = paths_for(tree.root() / "portable");
    const auto legacy = tree.root() / "appdata" / "HoyoFlux";
    const std::string config = R"(schema = 2
[launcher]
game = "genshin"
profile = "desktop"
[profiles.desktop]
game = "genshin"
fps = 120
)";
    write_file(legacy / "config.toml", config);
    // Model a process stopped after publishing the local config but before
    // source deletion. The resume record is deliberately checked against both
    // exact file hashes by production code.
    write_file(paths.config, config);
    const auto digest = win32::sha256_hex(config);
    REQUIRE(digest.has_value());
    write_file(paths.migration_record,
        "hoyoflux-portable-migration=1\n"
        "stage=prepared\n"
        "imported_config=1\n"
        "source_sha256=" + *digest + "\n"
        "target_sha256=" + *digest + "\n"
        "backup=appdata-config-" + *digest + ".toml\n");
    write_file(paths.backups / ("appdata-config-" + *digest + ".toml"), config);

    auto result = app::migrate_legacy_data(paths, legacy);
    REQUIRE(result.has_value());
    CHECK_FALSE(std::filesystem::exists(legacy / "config.toml"));
    CHECK_FALSE(std::filesystem::exists(paths.migration_record));
}

TEST_CASE("a changed AppData source stops a prepared migration without deletion",
          "[app][migration][resume]") {
    TempTree tree("resume-conflict");
    const auto paths = paths_for(tree.root() / "portable");
    const auto legacy = tree.root() / "appdata" / "HoyoFlux";
    const std::string original = "schema = 2\n[launcher]\n[profiles.desktop]\ngame = \"genshin\"\n";
    const std::string changed = "schema = 2\n[launcher]\n[profiles.desktop]\ngame = \"genshin\"\nfps = 60\n";
    write_file(legacy / "config.toml", changed);
    write_file(paths.config, original);
    const auto original_hash = win32::sha256_hex(original);
    REQUIRE(original_hash.has_value());
    write_file(paths.migration_record,
        "hoyoflux-portable-migration=1\n"
        "stage=prepared\n"
        "imported_config=1\n"
        "source_sha256=" + *original_hash + "\n"
        "target_sha256=" + *original_hash + "\n"
        "backup=appdata-config-" + *original_hash + ".toml\n");

    auto result = app::migrate_legacy_data(paths, legacy);
    REQUIRE_FALSE(result.has_value());
    CHECK(std::filesystem::exists(legacy / "config.toml"));
    CHECK(std::filesystem::exists(paths.config));
    CHECK(std::filesystem::exists(paths.migration_record));
}

TEST_CASE("failure diagnostics are read-only and persist beside the executable",
          "[app][diagnostics]") {
    TempTree tree("diagnostics");
    const auto paths = paths_for(tree.root() / "portable");
    const std::string invalid = "schema = 2\n[profiles.bad\n";
    write_file(paths.config, invalid);
    app::DiagnosticContext context;
    context.error = Error::make(ErrorCode::ConfigParseFailed,
                                "TOML parse error: test failure");
    context.full = false;
    REQUIRE(app::write_diagnostic_report(paths, context).has_value());
    CHECK(read_file(paths.config) == invalid);
    REQUIRE(std::filesystem::exists(paths.diagnostics));
    const auto report = read_file(paths.diagnostics);
    CHECK(report.find("TOML parse error: test failure") != std::string::npos);
    CHECK(report.find("不会迁移文件") != std::string::npos);
}
