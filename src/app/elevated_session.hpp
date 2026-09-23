#pragma once

#include "app/app_paths.hpp"
#include "profile/config.hpp"

#include <string>

namespace hoyoflux::app {

struct ElevatedSessionArguments {
    std::wstring token;
    std::wstring pipe_name;
};

// The normal process owns this route. It validates the local configuration,
// creates a one-use pipe, requests UAC, and receives the child result.
Result<int> launch_elevated_session(const AppPaths& paths,
                                    const profile::Config& confirmed_config);

// The UAC child only accepts an already-created local pipe/context. It never
// looks for AppData, opens editors, surfaces UI, or performs file migration.
Result<int> run_elevated_session_child(const AppPaths& paths,
                                       const ElevatedSessionArguments& arguments);

}  // namespace hoyoflux::app
