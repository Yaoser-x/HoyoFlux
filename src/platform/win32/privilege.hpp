#pragma once

// Administrator / elevation checks.

#include "domain/error.hpp"

#include <windows.h>

#include <string>

namespace hoyoflux::win32 {

// True when the current process is running elevated (admin).
bool is_elevated();

// Fails with ErrorCode::NotElevated when not running as Administrator.
Result<void> ensure_elevated();

// Canonical SID from the effective process token.  SIDs, not display names or
// environment variables, define the account boundary for UAC handshakes.
Result<std::wstring> current_user_sid();
Result<std::wstring> process_user_sid(DWORD process_id);

}  // namespace hoyoflux::win32
