#include "platform/win32/elevation.hpp"

#include "platform/win32/process.hpp"
#include "platform/win32/unique_handle.hpp"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>

namespace hoyoflux::win32 {
namespace {

Result<std::filesystem::path> current_executable_path() {
    std::wstring path(256, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, path.data(),
                                               static_cast<DWORD>(path.size()));
        if (size == 0) {
            return std::unexpected(Error::make(
                ErrorCode::OsError, "GetModuleFileNameW failed",
                GetLastError()));
        }
        if (size < path.size() - 1) {
            path.resize(size);
            return std::filesystem::path{std::move(path)};
        }
        path.resize(path.size() * 2);
    }
}

}  // namespace

Result<int> relaunch_elevated_and_wait(
    const std::vector<std::wstring>& arguments, ElevationResult* result) {
    if (result != nullptr) {
        *result = ElevationResult::Completed;
    }

    auto executable = current_executable_path();
    if (!executable) {
        return std::unexpected(executable.error());
    }

    const std::wstring parameters = build_command_line(arguments);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = executable->c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) {
            if (result != nullptr) {
                *result = ElevationResult::Cancelled;
            }
            return std::unexpected(Error::make(
                ErrorCode::ElevationCancelled, "elevation cancelled", error));
        }
        return std::unexpected(Error::make(
            ErrorCode::OsError, "ShellExecuteExW(runas) failed", error));
    }

    UniqueHandle child(info.hProcess);
    if (!child) {
        return std::unexpected(Error::make(
            ErrorCode::OsError,
            "ShellExecuteExW returned no child process handle"));
    }
    if (WaitForSingleObject(child.get(), INFINITE) == WAIT_FAILED) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "WaitForSingleObject(elevated child) failed",
            GetLastError()));
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(child.get(), &exit_code)) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "GetExitCodeProcess(elevated child) failed",
            GetLastError()));
    }
    return static_cast<int>(exit_code);
}

}  // namespace hoyoflux::win32
