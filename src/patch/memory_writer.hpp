#pragma once

// Remote memory primitives over plain Win32.
//
// Replaces the legacy NTSYSAPI write path (main.cpp:1199-1247) with the
// documented APIs: VirtualQueryEx -> VirtualProtectEx -> WriteProcessMemory
// -> restore. Reads never change protection. Allocations for rip-relative
// use walk downwards from an anchor so the +-2GB displacement reach holds.

#include "domain/error.hpp"
#include "platform/win32/unique_handle.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace hoyoflux::patch {

// Read `bytes.size()` bytes at `address`. The whole span must be readable.
Result<void> read_bytes(const win32::UniqueHandle& process, uintptr_t address,
                        std::span<std::byte> bytes);

// Write `data` to `address` with the query -> protect -> write -> restore
// flow. Restores the original protection even on a partial failure.
Result<void> write_protected(const win32::UniqueHandle& process, uintptr_t address,
                             std::span<const std::byte> data);

// Allocate `size` bytes (rounded up to a page) below `near_address`, walking
// down in page steps like the legacy basefps allocator
// (main.cpp:1517-1523). Returns the remote base address.
Result<uintptr_t> allocate_near(const win32::UniqueHandle& process,
                                uintptr_t near_address, size_t size);

// F5: like allocate_near but with PAGE_EXECUTE_READWRITE, for bootstrap
// stub code. Writes into the fresh page afterwards need no unprotect flow.
Result<uintptr_t> allocate_code_near(const win32::UniqueHandle& process,
                                     uintptr_t near_address, size_t size);

// Release a block returned by allocate_near. Never call while the game may
// still read the block.
Result<void> free_remote(const win32::UniqueHandle& process, uintptr_t address);

}  // namespace hoyoflux::patch
