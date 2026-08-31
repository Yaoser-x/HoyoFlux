#pragma once

// Process management on plain Win32: suspended spawn, snapshot enumeration,
// path query, liveness, terminate. This replaces the legacy NTSYSAPI
// process layer (GetPID / CreateProcessW shim) with direct Win32 calls.

#include "domain/error.hpp"
#include "platform/win32/unique_handle.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hoyoflux::win32 {

struct ProcessInfo {
    DWORD pid{0};
    std::wstring name;  // exe file name, e.g. L"YuanShen.exe"
    std::filesystem::path path;  // populated only when queried
};

// A freshly created, still-suspended process. The caller resumes the thread
// (LaunchedProcess::thread) once patching is done.
struct LaunchedProcess {
    UniqueHandle process;
    UniqueHandle thread;
    DWORD pid{0};
    DWORD tid{0};
};

// Encode one argv token as a Windows command-line argument, following
// CommandLineToArgvW rules exactly: spaces/tabs force quoting, embedded
// quotes are escaped, and backslash runs double up only when they precede a
// quote or end the argument. Empty tokens become "".
[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view arg);

// Build a Windows command line from argv-style tokens by encoding each one
// with quote_windows_argument and joining with single spaces. The result
// round-trips through CommandLineToArgvW back to `args`.
std::wstring build_command_line(const std::vector<std::wstring>& args);

// CreateProcessW with CREATE_SUSPENDED. `priority_class` may be 0 to keep the
// default (e.g. NORMAL_PRIORITY_CLASS).
Result<LaunchedProcess> spawn_suspended(const std::filesystem::path& exe_path,
                                        const std::vector<std::wstring>& args,
                                        const std::filesystem::path& working_dir,
                                        DWORD priority_class);

// Snapshot-based enumeration. `name_filter` is matched case-insensitively
// against the exe file name; an empty filter matches every process.
Result<std::vector<ProcessInfo>> enumerate_processes(std::wstring_view name_filter);

// First process whose exe file name matches (case-insensitive).
Result<std::optional<ProcessInfo>> find_process(std::wstring_view name);

// Full image path of an existing process via QueryFullProcessImageNameW.
Result<std::filesystem::path> query_process_path(const UniqueHandle& process);

// Open an existing process by pid.
Result<UniqueHandle> open_process(DWORD pid, DWORD access);

// True when a process with this pid is alive.
bool is_process_running(DWORD pid);

// Terminate and wait up to `timeout_ms` for it to exit.
Result<void> terminate_and_wait(const UniqueHandle& process, uint32_t timeout_ms);

// Resume the initially suspended process thread and report the documented
// ResumeThread failure sentinel instead of allowing the caller to wait on a
// thread that never started.
Result<void> resume_thread(const UniqueHandle& thread);

// Suspend / resume every thread of the process (Toolhelp thread snapshot +
// SuspendThread / ResumeThread). Balanced pairs only: resuming decrements
// each thread's suspend count once.
Result<void> suspend_process_threads(DWORD pid);
Result<void> resume_process_threads(DWORD pid);

// F5: run `start_address` in the remote process via CreateRemoteThread and
// return the thread handle (the caller waits and releases it through RAII).
// This is the minimal bootstrap invocation primitive - no DLL loading, no
// shellcode loader.
Result<UniqueHandle> create_remote_thread(const UniqueHandle& process,
                                          uintptr_t start_address);

}  // namespace hoyoflux::win32
