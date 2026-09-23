#pragma once

// Configuration-driven diagnostics: strictly read-only environment inspection.
// It never patches a game, never writes memory, and never changes state;
// when a game process happens to be running, its signatures are resolved
// live for the freshness report (read access only).

#include "app/app_paths.hpp"
#include "profile/config.hpp"

#include <optional>
#include <string>

namespace hoyoflux::app {

struct DiagnosticContext {
    std::optional<profile::Config> config;
    std::optional<Error> error;
    std::string migration_state;
    bool full{false};
};

Result<std::string> build_diagnostic_report(
    const AppPaths& paths, const DiagnosticContext& context = {});
Result<void> write_diagnostic_report(
    const AppPaths& paths, const DiagnosticContext& context = {});

}  // namespace hoyoflux::app
