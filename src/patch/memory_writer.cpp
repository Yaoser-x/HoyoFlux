#include "patch/memory_writer.hpp"

#include <memoryapi.h>
#include <minwinbase.h>

#include <array>
#include <cstring>
#include <string>

namespace hoyoflux::patch {
namespace {

constexpr size_t kPageSize = 0x1000;
constexpr uintptr_t kMaxNearWalk = 0x7FFF8000;  // legacy main.cpp:1519

[[nodiscard]] bool page_writable(uint32_t protect) noexcept {
    switch (protect & 0xFF) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

}  // namespace

Result<void> read_bytes(const win32::UniqueHandle& process, uintptr_t address,
                        std::span<std::byte> bytes) {
    if (bytes.empty()) {
        return {};
    }
    SIZE_T read = 0;
    if (!ReadProcessMemory(process.get(), reinterpret_cast<LPCVOID>(address),
                           bytes.data(), bytes.size(), &read) ||
        read != bytes.size()) {
        return std::unexpected(Error::make(
            ErrorCode::ReadProcessMemoryFailed,
            "ReadProcessMemory failed at " + std::to_string(address) + " (" +
                std::to_string(bytes.size()) + " bytes)",
            GetLastError()));
    }
    return {};
}

Result<void> write_protected(const win32::UniqueHandle& process, uintptr_t address,
                             std::span<const std::byte> data) {
    if (data.empty()) {
        return {};
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(process.get(), reinterpret_cast<LPCVOID>(address), &mbi,
                       sizeof(mbi)) != sizeof(mbi)) {
        return std::unexpected(Error::make(
            ErrorCode::PatchFailed,
            "VirtualQueryEx failed for " + std::to_string(address), GetLastError()));
    }
    if (mbi.State != MEM_COMMIT) {
        return std::unexpected(Error::make(
            ErrorCode::PatchFailed,
            "target page at " + std::to_string(address) + " is not committed"));
    }

    // .text is typically PAGE_EXECUTE_READ; the game may also have guarded
    // pages. Take a writeable protection, write, then restore what was there.
    const uint32_t original_protect = mbi.Protect;
    DWORD old_protect = 0;
    const bool need_unprotect = !page_writable(original_protect);
    if (need_unprotect &&
        !VirtualProtectEx(process.get(), reinterpret_cast<LPVOID>(address),
                          data.size(), PAGE_READWRITE, &old_protect)) {
        return std::unexpected(Error::make(
            ErrorCode::PatchFailed,
            "VirtualProtectEx failed for " + std::to_string(address),
            GetLastError()));
    }

    SIZE_T written = 0;
    const bool wrote = WriteProcessMemory(
        process.get(), reinterpret_cast<LPVOID>(address), data.data(), data.size(),
        &written);

    if (need_unprotect) {
        DWORD ignored = 0;
        VirtualProtectEx(process.get(), reinterpret_cast<LPVOID>(address),
                         data.size(), original_protect, &ignored);
    }
    if (!wrote || written != data.size()) {
        return std::unexpected(Error::make(
            ErrorCode::RemoteWriteFailed,
            "WriteProcessMemory failed at " + std::to_string(address) + " (" +
                std::to_string(data.size()) + " bytes)",
            GetLastError()));
    }
    return {};
}

namespace {

// Shared walk used by both allocation flavors.
Result<uintptr_t> allocate_near_protect(const win32::UniqueHandle& process,
                                        uintptr_t near_address, size_t size,
                                        DWORD protect) {
    const size_t block = (size + kPageSize - 1) & ~(kPageSize - 1);
    if (block == 0 || block > 0x10000000) {
        return std::unexpected(
            Error::make(ErrorCode::InvalidArgument, "unreasonable allocation size"));
    }
    for (uintptr_t offset = kPageSize * 16; offset < kMaxNearWalk;
         offset += kPageSize) {
        const uintptr_t candidate = (near_address > offset + block)
                                        ? near_address - offset
                                        : 0;
        if (candidate == 0) {
            break;
        }
        LPVOID allocated = VirtualAllocEx(
            process.get(), reinterpret_cast<LPVOID>(candidate), block,
            MEM_COMMIT | MEM_RESERVE, protect);
        if (allocated != nullptr) {
            return reinterpret_cast<uintptr_t>(allocated);
        }
        // ERROR_INVALID_ADDRESS means something else owns the page: keep
        // walking. Anything else is a real failure worth reporting.
        const DWORD err = GetLastError();
        if (err != ERROR_INVALID_ADDRESS && err != ERROR_COMMITMENT_LIMIT) {
            return std::unexpected(Error::make(
                ErrorCode::RemoteAllocFailed,
                "VirtualAllocEx failed near " + std::to_string(candidate), err));
        }
    }
    return std::unexpected(Error::make(
        ErrorCode::RemoteAllocFailed,
        "no free page below " + std::to_string(near_address)));
}

}  // namespace

Result<uintptr_t> allocate_near(const win32::UniqueHandle& process,
                                uintptr_t near_address, size_t size) {
    return allocate_near_protect(process, near_address, size, PAGE_READWRITE);
}

Result<uintptr_t> allocate_code_near(const win32::UniqueHandle& process,
                                     uintptr_t near_address, size_t size) {
    return allocate_near_protect(process, near_address, size,
                                 PAGE_EXECUTE_READWRITE);
}

Result<void> write_u32(const win32::UniqueHandle& process, uintptr_t address,
                       uint32_t value) {
    std::array<std::byte, 4> raw{};
    std::memcpy(raw.data(), &value, sizeof(value));
    return write_protected(process, address, raw);
}

Result<void> free_remote(const win32::UniqueHandle& process, uintptr_t address) {
    if (!VirtualFreeEx(process.get(), reinterpret_cast<LPVOID>(address), 0,
                       MEM_RELEASE)) {
        return std::unexpected(
            Error::make(ErrorCode::RemoteAllocFailed,
                        "VirtualFreeEx failed for " + std::to_string(address),
                        GetLastError()));
    }
    return {};
}

}  // namespace hoyoflux::patch
