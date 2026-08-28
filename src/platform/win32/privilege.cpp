#include "platform/win32/privilege.hpp"

#include <windows.h>

#include "platform/win32/unique_handle.hpp"

namespace hoyoflux::win32 {

bool is_elevated() {
    UniqueHandle token;
    {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
            return false;
        }
        token = UniqueHandle(raw);
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation,
                             sizeof(elevation), &size)) {
        return false;
    }
    return elevation.TokenIsElevated != 0;
}

Result<void> ensure_elevated() {
    if (is_elevated()) {
        return {};
    }
    return std::unexpected(Error::make(
        ErrorCode::NotElevated,
        "HoyoFlux requires an elevated (Administrator) process to launch games"));
}

}  // namespace hoyoflux::win32
