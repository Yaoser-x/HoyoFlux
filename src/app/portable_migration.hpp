#pragma once

#include "app/app_paths.hpp"

namespace hoyoflux::app {

struct MigrationResult {
    bool imported_config{false};
    bool imported_journal{false};
    bool removed_legacy_config{false};
};

// Moves recognized v1 AppData files into the portable directory. The explicit
// legacy_root parameter keeps tests isolated from the user's real AppData.
Result<MigrationResult> migrate_legacy_data(
    const AppPaths& paths,
    const std::filesystem::path& legacy_root);

}  // namespace hoyoflux::app
