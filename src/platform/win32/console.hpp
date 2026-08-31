#pragma once

#include <windows.h>

namespace hoyoflux::win32 {

// Attach explicit CLI invocations to their launching terminal. Failure means
// the caller was started without a parent console and is intentionally quiet.
bool attach_parent_console();

// Attach to a specific console owner process. This is used by an elevated
// child because the UAC broker is not necessarily the original console
// parent.
bool attach_console(DWORD process_id);

}  // namespace hoyoflux::win32
