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

// Build a Windows command line from argv-style tokens (quoting rules follow
// CommandLineToArgvW semantics for the simple cases; documented in .cpp).
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

// Suspend / resume every thread of the process (Toolhelp thread snapshot +
// SuspendThread / ResumeThread). Balanced pairs only: resuming decrements
// each thread's suspend count once.
Result<void> suspend_process_threads(DWORD pid);
Result<void> resume_process_threads(DWORD pid);

}  // namespace hoyoflux::win32
