#pragma once

// Portable TOML profile store. Schema 2 keeps profile settings flat while the
// loader still accepts schema 1 long enough to migrate it atomically.

#include "domain/error.hpp"
#include "domain/game.hpp"
#include "domain/profile.hpp"
#include "platform/win32/display.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hoyoflux::profile {

enum class LauncherRegion { Auto, Cn, Global };
enum class LauncherAction { Launch, Diagnose };

struct LauncherConfig {
    GameId game{GameId::Genshin};
    std::string profile{"auto"};
    LauncherRegion region{LauncherRegion::Auto};
    LauncherAction action{LauncherAction::Launch};
    bool notifications{true};
};

struct Config {
    std::vector<Profile> profiles;
    std::string default_profile;  // populated only while reading schema 1
    std::string genshin_default{"desktop"};
    std::string starrail_default{"starrail_desktop"};
    LauncherConfig launcher;
    int schema{2};
};

// A validated schema-1 conversion held entirely in memory.  Preparing this
// object has no filesystem effects; callers may inspect it before committing
// a migration while holding their own session/migration lock.
struct ConfigMigrationPreparation {
    Config source;
    Config converted;
    std::string source_text;
    std::string converted_text;
};

struct DisplayFacts {
    win32::DisplayInfo info;
    Resolution resolution{0, 0};
    uint32_t refresh_rate{0};
    float aspect_ratio{0.0f};
    bool portrait{false};
};

struct AutoCandidateDecision {
    std::string profile_id;
    std::optional<uint32_t> display_index;
    int specificity{-1};
    int priority{0};
};

struct AutoProfileDecision {
    Profile profile;
    bool used_fallback{false};
    std::optional<uint32_t> display_index;
    int specificity{0};
    int priority{0};
    std::vector<DisplayFacts> displays;
    std::vector<AutoCandidateDecision> candidates;
};

// The document written when no config file exists (also the documentation).
[[nodiscard]] std::string default_config_toml();

// Schema 2 rejects unknown keys and reports source locations where available.
// base_directory resolves relative per-profile executable paths.
Result<Config> parse_config(
    std::string_view toml_text,
    const std::filesystem::path& base_directory = {});

// Read and validate a config without creating a backup, converting schema 1,
// or changing timestamps/content. Missing files are errors. The application
// materializes defaults explicitly so first run can stop before launching.
Result<Config> read_config(const std::filesystem::path& path);

// Parses only the valid TOML launcher section to decide whether an explicit
// diagnostic request can proceed even when another profile has a validation
// error. It never guesses from malformed text and never writes a file.
Result<LauncherAction> read_declared_launcher_action(
    const std::filesystem::path& path);

// Build and commit the schema-1 to schema-2 conversion explicitly. Commit
// archives the original, publishes the converted bytes, rereads them from
// disk, and compares their effective behavior with the prepared source.
Result<ConfigMigrationPreparation> prepare_config_migration(
    const std::filesystem::path& path);
Result<Config> commit_config_migration(
    const std::filesystem::path& path,
    const std::filesystem::path& backup_directory,
    const ConfigMigrationPreparation& preparation);
Result<Config> migrate_config_file(const std::filesystem::path& path,
                                   const std::filesystem::path& backup_directory = {});

Result<void> write_default_config(const std::filesystem::path& path);

// Canonical schema-2 output used by the legacy migration.
[[nodiscard]] std::string serialize_config(const Config& config);
[[nodiscard]] bool configs_equivalent(const Config& left, const Config& right);

// Find a profile by id.
Result<Profile> find_profile(const Config& config, std::string_view id);

// With launcher.profile = "auto", pick the profile for `game` from attached
// displays.
// Heuristic: with a portrait display attached, the first mobile-UI profile
// for the game; otherwise the first non-mobile profile for the game.
Result<Profile> match_auto_profile(const Config& config, GameId game,
                                   const std::vector<win32::DisplayInfo>& displays);

Result<AutoProfileDecision> resolve_auto_profile(
    const Config& config, GameId game,
    const std::vector<win32::DisplayInfo>& displays);

}  // namespace hoyoflux::profile
