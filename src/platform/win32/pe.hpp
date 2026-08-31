#pragma once

// PE image parsing (headers, section table, RVA -> VA mapping).
//
// Replaces the legacy Get_Section_info() (main.cpp:744-776) which compared
// section names as raw uint64 and trusted VirtualSize without validation.
// Every offset here is bounds-checked against the provided buffer.

#include "domain/error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hoyoflux::win32 {

struct PeSection {
    std::string name;
    uint32_t virtual_address{0};   // RVA of the section
    uint32_t virtual_size{0};      // Misc.VirtualSize
    uint32_t size_of_raw_data{0};
    uint32_t pointer_to_raw_data{0};
    uint32_t characteristics{0};

    [[nodiscard]] bool is_readable() const noexcept;
    [[nodiscard]] bool is_executable() const noexcept;
};

struct PeInfo {
    uint16_t machine{0};
    uintptr_t image_base{0};
    uint32_t size_of_image{0};
    uint32_t section_alignment{0};
    std::vector<PeSection> sections;

    // Section lookup by exact name (e.g. ".text", "il2cpp").
    [[nodiscard]] const PeSection* find_section(std::string_view name) const noexcept;
};

// Parse a PE image from an in-memory buffer. The buffer may contain only the
// header + section table; every read is bounds-checked.
Result<PeInfo> parse_pe(std::span<const std::byte> image);

// Map an RVA to an absolute address given a module base. Returns nullopt when
// the RVA does not fall inside any declared section.
std::optional<uintptr_t> rva_to_va(const PeInfo& pe, uintptr_t module_base,
                                   uint32_t rva) noexcept;

}  // namespace hoyoflux::win32
