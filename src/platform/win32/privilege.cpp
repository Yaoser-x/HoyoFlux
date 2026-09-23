#include "platform/win32/privilege.hpp"

#include <windows.h>
#include <sddl.h>

#include <cstddef>
#include <vector>

#include "platform/win32/unique_handle.hpp"

namespace hoyoflux::win32 {
namespace {

Result<std::wstring> sid_from_process(HANDLE process) {
    UniqueHandle token;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "OpenProcessToken failed", GetLastError()));
    }
    token = UniqueHandle(raw);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "GetTokenInformation(TokenUser) size failed", GetLastError()));
    }
    std::vector<std::byte> buffer(size);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "GetTokenInformation(TokenUser) failed", GetLastError()));
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid) || sid == nullptr) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "ConvertSidToStringSidW failed", GetLastError()));
    }
    std::wstring result(sid);
    LocalFree(sid);
    return result;
}

}  // namespace

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

Result<std::wstring> current_user_sid() {
    return sid_from_process(GetCurrentProcess());
}

Result<std::wstring> process_user_sid(DWORD process_id) {
    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                    process_id));
    if (!process) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "OpenProcess for SID check failed", GetLastError()));
    }
    return sid_from_process(process.get());
}

}  // namespace hoyoflux::win32
