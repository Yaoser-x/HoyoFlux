#include "platform/win32/process.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <cwctype>
#include <string>
#include <utility>

namespace hoyoflux::win32 {
namespace {

bool iequals_ascii(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) {
            return false;
        }
    }
    return true;
}

Error win32_error(ErrorCode code, std::string_view what) {
    return Error::make(code, std::string(what), GetLastError());
}

}  // namespace

std::wstring build_command_line(const std::vector<std::wstring>& args) {
    std::wstring cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& arg = args[i];
        const bool needs_quotes = arg.empty() ||
                                  arg.find_first_of(L" \t\"") != std::wstring::npos;
        if (needs_quotes) {
            cmd += L'"';
            for (const wchar_t ch : arg) {
                if (ch == L'"') {
                    cmd += L"\\\"";
                } else {
                    cmd += ch;
                }
            }
            cmd += L'"';
        } else {
            cmd += arg;
        }
        if (i + 1 < args.size()) {
            cmd += L' ';
        }
    }
    return cmd;
}

Result<LaunchedProcess> spawn_suspended(const std::filesystem::path& exe_path,
                                        const std::vector<std::wstring>& args,
                                        const std::filesystem::path& working_dir,
                                        DWORD priority_class) {
    // argv[0] is conventionally the executable itself.
    std::vector<std::wstring> full_args;
    full_args.reserve(args.size() + 1);
    full_args.push_back(L"\"" + exe_path.wstring() + L"\"");
    full_args.insert(full_args.end(), args.begin(), args.end());
    std::wstring command_line = build_command_line(full_args);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    DWORD flags = CREATE_SUSPENDED;
    if (priority_class != 0) {
        flags |= priority_class;
    }

    const BOOL ok = CreateProcessW(
        exe_path.c_str(),                      // lpApplicationName
        command_line.data(),                   // lpCommandLine (writable)
        nullptr, nullptr, FALSE, flags, nullptr,
        working_dir.empty() ? nullptr : working_dir.c_str(), &si, &pi);
    if (!ok) {
        return std::unexpected(
            win32_error(ErrorCode::ProcessSpawnFailed,
                        "CreateProcessW failed for " + exe_path.string()));
    }

    LaunchedProcess lp;
    lp.process.reset(pi.hProcess);
    lp.thread.reset(pi.hThread);
    lp.pid = pi.dwProcessId;
    lp.tid = pi.dwThreadId;
    return lp;
}

Result<std::vector<ProcessInfo>> enumerate_processes(std::wstring_view name_filter) {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return std::unexpected(
            win32_error(ErrorCode::OsError, "CreateToolhelp32Snapshot failed"));
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        if (GetLastError() == ERROR_NO_MORE_FILES) {
            return std::vector<ProcessInfo>{};
        }
        return std::unexpected(
            win32_error(ErrorCode::OsError, "Process32FirstW failed"));
    }

    std::vector<ProcessInfo> out;
    do {
        if (name_filter.empty() || iequals_ascii(entry.szExeFile, name_filter)) {
            ProcessInfo info;
            info.pid = entry.th32ProcessID;
            info.name = entry.szExeFile;
            out.push_back(std::move(info));
        }
    } while (Process32NextW(snapshot.get(), &entry));
    return out;
}

Result<std::optional<ProcessInfo>> find_process(std::wstring_view name) {
    auto procs = enumerate_processes(name);
    if (!procs) {
        return std::unexpected(procs.error());
    }
    if (procs->empty()) {
        return std::optional<ProcessInfo>{};
    }
    return procs->front();
}

Result<std::filesystem::path> query_process_path(const UniqueHandle& process) {
    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &size)) {
        return std::unexpected(
            win32_error(ErrorCode::OsError, "QueryFullProcessImageNameW failed"));
    }
    buffer.resize(size);
    return std::filesystem::path{std::move(buffer)};
}

Result<UniqueHandle> open_process(DWORD pid, DWORD access) {
    HANDLE h = OpenProcess(access, FALSE, pid);
    if (h == nullptr) {
        return std::unexpected(
            win32_error(ErrorCode::OsError, "OpenProcess failed"));
    }
    return UniqueHandle(h);
}

bool is_process_running(DWORD pid) {
    UniqueHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                               FALSE, pid));
    if (!h) {
        return false;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(h.get(), &exit_code)) {
        return true;  // cannot tell; assume alive
    }
    return exit_code == STILL_ACTIVE;
}

Result<void> terminate_and_wait(const UniqueHandle& process, uint32_t timeout_ms) {
    if (!process) {
        return std::unexpected(
            Error::make(ErrorCode::InvalidArgument, "terminate_and_wait: no process"));
    }
    if (!TerminateProcess(process.get(), 0)) {
        return std::unexpected(
            win32_error(ErrorCode::OsError, "TerminateProcess failed"));
    }
    WaitForSingleObject(process.get(), timeout_ms);
    return {};
}

namespace {

// Walk every thread of `pid` and apply `fn` (SuspendThread / ResumeThread),
// counting successful calls. A thread that exits while we walk is skipped.
Result<int> walk_threads(DWORD pid, DWORD(WINAPI* fn)(HANDLE)) {
    win32::UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snapshot) {
        return std::unexpected(
            win32_error(ErrorCode::OsError, "CreateToolhelp32Snapshot(threads) failed"));
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot.get(), &entry)) {
        return std::unexpected(win32_error(ErrorCode::OsError, "Thread32First failed"));
    }
    int affected = 0;
    do {
        if (entry.th32OwnerProcessID != pid) {
            continue;
        }
        UniqueHandle thread(OpenThread(THREAD_SUSPEND_RESUME, FALSE,
                                       entry.th32ThreadID));
        if (!thread) {
            continue;  // exited between snapshot and open
        }
        if (fn(thread.get()) != static_cast<DWORD>(-1)) {
            ++affected;
        }
    } while (Thread32Next(snapshot.get(), &entry));
    return affected;
}

}  // namespace

Result<void> suspend_process_threads(DWORD pid) {
    auto suspended = walk_threads(pid, SuspendThread);
    if (!suspended) {
        return std::unexpected(suspended.error());
    }
    if (*suspended == 0) {
        return std::unexpected(Error::make(ErrorCode::OsError,
                                           "no threads suspended (process gone?)"));
    }
    return {};
}

Result<void> resume_process_threads(DWORD pid) {
    auto resumed = walk_threads(pid, ResumeThread);
    if (!resumed) {
        return std::unexpected(resumed.error());
    }
    if (*resumed == 0) {
        return std::unexpected(Error::make(ErrorCode::OsError,
                                           "no threads resumed (process gone?)"));
    }
    return {};
}

}  // namespace hoyoflux::win32
