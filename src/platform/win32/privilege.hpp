#pragma once

// Administrator / elevation checks.

#include "domain/error.hpp"

namespace hoyoflux::win32 {

// True when the current process is running elevated (admin).
bool is_elevated();

// Fails with ErrorCode::NotElevated when not running as Administrator.
Result<void> ensure_elevated();

}  // namespace hoyoflux::win32
