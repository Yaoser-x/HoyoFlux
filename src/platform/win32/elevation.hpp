#pragma once

// On-demand UAC bootstrap for the launch path. The executable remains
// `asInvoker`; first-run setup and diagnostics never call this boundary.

#include "domain/error.hpp"

#include <windows.h>

#include <string>
#include <string_view>
#include <functional>
#include <vector>

namespace hoyoflux::win32 {

enum class ElevationResult { Completed, Cancelled };

// Private bootstrap marker. It is accepted only for the UAC child process and
// is not a public command-line interface.
inline constexpr std::wstring_view kInternalElevatedArgument =
    L"--hoyoflux-internal-elevated";
inline constexpr std::wstring_view kInternalPipeArgument =
    L"--hoyoflux-internal-pipe";

// Re-launch this executable with the supplied argv-style arguments using the
// UAC `runas` verb, wait for the child, and return its exit code unchanged.
// `result` is set to Cancelled only for an explicit ERROR_CANCELLED consent
// response; other failures are returned as ordinary errors.
Result<int> relaunch_elevated_and_wait(
    const std::vector<std::wstring>& arguments,
    ElevationResult* result = nullptr,
    const std::function<Result<void>(DWORD)>& after_launch = {});

}  // namespace hoyoflux::win32
