#pragma once

namespace hoyoflux::win32 {

// Attach explicit CLI invocations to their launching terminal. Failure means
// the caller was started without a parent console and is intentionally quiet.
bool attach_parent_console();

}  // namespace hoyoflux::win32
