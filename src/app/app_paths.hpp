#pragma once

#include "domain/error.hpp"

#include <filesystem>

namespace hoyoflux::app {

struct AppPaths {
    std::filesystem::path executable;
    std::filesystem::path root;
    std::filesystem::path config;
    std::filesystem::path data;
    std::filesystem::path state;
    std::filesystem::path journal;
    std::filesystem::path migration_record;
    std::filesystem::path backups;
    std::filesystem::path diagnostics;
};

// Resolves every user-owned path from the running executable, never from the
// process working directory.
Result<AppPaths> resolve_app_paths();

// Creates the portable data directories and proves that both the application
// root and data directory are writable. No AppData fallback is permitted.
Result<void> prepare_portable_directories(const AppPaths& paths);

// Legacy v1 locations used only by the one-time portable migration.
Result<std::filesystem::path> legacy_appdata_root();

}  // namespace hoyoflux::app
