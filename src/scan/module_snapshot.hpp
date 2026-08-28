#pragma once

// Remote module snapshot: read a module's headers and the specific sections
// a game needs to pattern-scan, once per session.
//
// The legacy flow read whole images (or trusted VirtualSize blindly) and
// re-read the same sections for every signature. This module reads each
// requested section exactly once into a local buffer that every signature
// then shares (plan §17).

#include "domain/error.hpp"
#include "platform/win32/pe.hpp"
#include "platform/win32/unique_handle.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hoyoflux::scan {

// One locally-copied section, ready for pattern scanning.
struct SectionCopy {
    std::string name;
    uint32_t rva{0};             // section VirtualAddress within the module
    uintptr_t remote_address{0}; // module_base + rva
    std::vector<std::byte> bytes;
};

struct ModuleSnapshot {
    uintptr_t module_base{0};
    win32::PeInfo pe;
    std::vector<SectionCopy> sections;

    // Locate a copied section; nullptr when not requested or not present.
    [[nodiscard]] const SectionCopy* find_section(std::string_view name) const noexcept;

    // Translate a local offset inside `section` to the remote absolute address.
    [[nodiscard]] uintptr_t local_to_remote(const SectionCopy& section,
                                            size_t offset) const noexcept;
};

// Base address of a process's main module, via Toolhelp module snapshot.
Result<uintptr_t> remote_module_base(const win32::UniqueHandle& process);

// Snapshot the module at `module_base`: parse headers and read each requested
// section into a local copy. Sections whose names are absent from the image
// are silently skipped (the caller checks find_section). Guards against
// absurd virtual sizes and failed reads.
Result<ModuleSnapshot> snapshot_module(const win32::UniqueHandle& process,
                                       uintptr_t module_base,
                                       std::span<const std::string_view> names);

}  // namespace hoyoflux::scan
