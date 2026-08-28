#include "platform/win32/pe.hpp"

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <utility>

namespace hoyoflux::win32 {

namespace {

std::string section_name(const unsigned char name[8]) {
    size_t len = 0;
    while (len < 8 && name[len] != '\0') {
        ++len;
    }
    return std::string(reinterpret_cast<const char*>(name), len);
}

std::unexpected<Error> pe_error(std::string_view why) {
    return std::unexpected(Error::make(ErrorCode::InvalidPe, std::string(why)));
}

}  // namespace

bool PeSection::is_readable() const noexcept {
    return (characteristics & IMAGE_SCN_MEM_READ) != 0;
}

bool PeSection::is_executable() const noexcept {
    return (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
}

const PeSection* PeInfo::find_section(std::string_view name) const noexcept {
    for (const auto& section : sections) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

Result<PeInfo> parse_pe(std::span<const std::byte> image) {
    const std::byte* base = image.data();
    const size_t size = image.size();

    if (size < sizeof(IMAGE_DOS_HEADER)) {
        return pe_error("buffer too small for DOS header");
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return pe_error("bad DOS signature");
    }

    const uint32_t pe_offset = dos->e_lfanew;
    if (pe_offset + 4 + sizeof(IMAGE_FILE_HEADER) > size) {
        return pe_error("PE headers out of range");
    }
    if (std::memcmp(base + pe_offset, "PE\0\0", 4) != 0) {
        return pe_error("bad PE signature");
    }

    const auto* file_header =
        reinterpret_cast<const IMAGE_FILE_HEADER*>(base + pe_offset + 4);

    PeInfo info;
    info.machine = file_header->Machine;

    const uint32_t opt_offset = pe_offset + 4 + sizeof(IMAGE_FILE_HEADER);
    if (file_header->SizeOfOptionalHeader < 2 ||
        opt_offset + file_header->SizeOfOptionalHeader > size) {
        return pe_error("optional header out of range");
    }

    const auto magic =
        *reinterpret_cast<const uint16_t*>(base + opt_offset);
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        const auto* opt =
            reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(base + opt_offset);
        info.image_base = opt->ImageBase;
        info.size_of_image = opt->SizeOfImage;
        info.section_alignment = opt->SectionAlignment;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const auto* opt =
            reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(base + opt_offset);
        info.image_base = static_cast<uintptr_t>(opt->ImageBase);
        info.size_of_image = opt->SizeOfImage;
        info.section_alignment = opt->SectionAlignment;
    } else {
        return pe_error("unknown optional header magic");
    }

    const uint32_t section_offset = opt_offset + file_header->SizeOfOptionalHeader;
    const size_t section_bytes =
        static_cast<size_t>(file_header->NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (section_offset + section_bytes > size) {
        return pe_error("section table out of range");
    }

    const auto* section_table =
        reinterpret_cast<const IMAGE_SECTION_HEADER*>(base + section_offset);
    info.sections.reserve(file_header->NumberOfSections);
    for (uint16_t i = 0; i < file_header->NumberOfSections; ++i) {
        PeSection section;
        section.name = section_name(section_table[i].Name);
        section.virtual_address = section_table[i].VirtualAddress;
        section.virtual_size = section_table[i].Misc.VirtualSize;
        section.size_of_raw_data = section_table[i].SizeOfRawData;
        section.pointer_to_raw_data = section_table[i].PointerToRawData;
        section.characteristics = section_table[i].Characteristics;
        info.sections.push_back(std::move(section));
    }
    return info;
}

std::optional<uintptr_t> rva_to_va(const PeInfo& pe, uintptr_t module_base,
                                   uint32_t rva) noexcept {
    for (const auto& section : pe.sections) {
        if (rva >= section.virtual_address &&
            rva < section.virtual_address + section.virtual_size) {
            return module_base + rva;
        }
    }
    return std::nullopt;
}

}  // namespace hoyoflux::win32
