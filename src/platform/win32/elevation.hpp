#pragma once

// On-demand UAC bootstrap for the launch path. The executable remains
// `asInvoker`; read-only commands never call this boundary.

#include "domain/error.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace hoyoflux::win32 {

enum class ElevationResult { Completed, Cancelled };

// Private bootstrap markers. application_main strips them before CLI11 sees
// the command line, so they are not part of the public CLI schema or help.
inline constexpr std::wstring_view kInternalElevatedArgument =
    L"--hoyoflux-internal-elevated";
inline constexpr std::wstring_view kConsoleOwnerArgumentPrefix =
    L"--hoyoflux-console-owner=";

// Re-launch this executable with the supplied argv-style arguments using the
// UAC `runas` verb, wait for the child, and return its exit code unchanged.
// `result` is set to Cancelled only for an explicit ERROR_CANCELLED consent
// response; other failures are returned as ordinary errors.
Result<int> relaunch_elevated_and_wait(
    const std::vector<std::wstring>& arguments,
    ElevationResult* result = nullptr);

}  // namespace hoyoflux::win32
