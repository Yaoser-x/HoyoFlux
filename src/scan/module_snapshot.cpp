#include "scan/module_snapshot.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <cstddef>
#include <cwchar>
#include <string>
#include <utility>

namespace hoyoflux::scan {
namespace {

constexpr size_t kPeHeaderSize = 0x1000;
// Defensive cap: no game section we scan is anywhere near this.
constexpr size_t kMaxSectionBytes = 512u * 1024u * 1024u;

Error win32_error(ErrorCode code, std::string_view what) {
    return Error::make(code, std::string(what), GetLastError());
}

}  // namespace

const SectionCopy* ModuleSnapshot::find_section(std::string_view name) const noexcept {
    for (const auto& section : sections) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

uintptr_t ModuleSnapshot::local_to_remote(const SectionCopy& section,
                                          size_t offset) const noexcept {
    return section.remote_address + offset;
}

Result<uintptr_t> remote_module_base(const win32::UniqueHandle& process) {
    const DWORD pid = GetProcessId(process.get());
    if (pid == 0) {
        return std::unexpected(win32_error(ErrorCode::OsError, "GetProcessId failed"));
    }

    win32::UniqueHandle snapshot(
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) {
        return std::unexpected(
            win32_error(ErrorCode::OsError, "CreateToolhelp32Snapshot failed"));
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.get(), &entry)) {
        return std::unexpected(
            win32_error(ErrorCode::ModuleNotFound, "Module32FirstW failed"));
    }
    return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
}

Result<uintptr_t> remote_module_base(const win32::UniqueHandle& process,
                                     std::string_view module_name) {
    const DWORD pid = GetProcessId(process.get());
    if (pid == 0) {
        return std::unexpected(win32_error(ErrorCode::OsError, "GetProcessId failed"));
    }

    win32::UniqueHandle snapshot(
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) {
        return std::unexpected(
            win32_error(ErrorCode::OsError, "CreateToolhelp32Snapshot failed"));
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.get(), &entry)) {
        return std::unexpected(
            win32_error(ErrorCode::ModuleNotFound, "Module32FirstW failed"));
    }
    do {
        // Case-insensitive compare against the module file name, e.g.
        // "UserAssembly.dll" (module name matching follows the loader).
        // ModuleRequirement names are ASCII; widen for the Win32 compare.
        const std::wstring wide_name(module_name.begin(), module_name.end());
        if (_wcsicmp(entry.szModule, wide_name.c_str()) == 0) {
            return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        }
    } while (Module32NextW(snapshot.get(), &entry));
    return std::unexpected(Error::make(ErrorCode::ModuleNotFound,
                                       "module not loaded: " +
                                           std::string(module_name)));
}

Result<ModuleSnapshot> snapshot_module(const win32::UniqueHandle& process,
                                       uintptr_t module_base,
                                       std::span<const std::string_view> names) {
    std::vector<std::byte> header(kPeHeaderSize);
    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(process.get(), reinterpret_cast<LPCVOID>(module_base),
                           header.data(), header.size(), &bytes_read) ||
        bytes_read != header.size()) {
        return std::unexpected(
            win32_error(ErrorCode::ReadProcessMemoryFailed,
                        "read of PE header failed"));
    }

    auto pe = win32::parse_pe(std::span<const std::byte>(header));
    if (!pe) {
        return std::unexpected(pe.error());
    }

    ModuleSnapshot snapshot;
    snapshot.module_base = module_base;
    snapshot.pe = *pe;

    for (const std::string_view name : names) {
        const auto* section = pe->find_section(name);
        if (section == nullptr) {
            continue;  // optional section absent from this image
        }

        size_t size = section->virtual_size;
        if (size == 0) {
            size = section->size_of_raw_data;
        }
        if (size == 0 || size > kMaxSectionBytes) {
            continue;  // guard against corrupt/unreasonable sizes
        }

        SectionCopy copy;
        copy.name = section->name;
        copy.rva = section->virtual_address;
        copy.remote_address = module_base + section->virtual_address;
        copy.bytes.resize(size);

        if (!ReadProcessMemory(process.get(),
                               reinterpret_cast<LPCVOID>(copy.remote_address),
                               copy.bytes.data(), copy.bytes.size(), &bytes_read) ||
            bytes_read != copy.bytes.size()) {
            return std::unexpected(Error::make(
                ErrorCode::ReadProcessMemoryFailed,
                "section read failed: " + section->name, GetLastError()));
        }
        snapshot.sections.push_back(std::move(copy));
    }
    return snapshot;
}

}  // namespace hoyoflux::scan
