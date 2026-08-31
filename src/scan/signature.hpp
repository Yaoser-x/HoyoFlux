#pragma once

// Data-ified signatures: one compiled pattern plus one or more resolvers that
// turn a match into a usable address/value, exactly reproducing the legacy
// per-version arithmetic (main.cpp) without scattering hex strings and
// inline pointer math across the adapter code.

#include "domain/error.hpp"
#include "scan/compiled_pattern.hpp"
#include "scan/module_snapshot.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace hoyoflux::scan {

enum class ResolveStrategy {
    Direct,        // resolved = found (the match address itself)
    FieldDisp,     // resolved = found + disp_offset  (address of a rip-rel disp field)
    RipRelative,   // resolved = found + disp_offset + skip + *(i32)(found + disp_offset)
    RawInt32At,    // resolved = (uintptr_t)(uint32_t)*(i32)(found + disp_offset)
};

struct ResolveSpec {
    ResolveStrategy strategy{ResolveStrategy::Direct};
    int64_t disp_offset{0};
    int64_t skip{0};
};

struct Signature {
    std::string_view id;
    std::string_view section;  // section name to scan (".text", "il2cpp")
    CompiledPattern pattern;
    std::vector<ResolveSpec> resolvers;  // >= 1; several for multi-field patterns
};

// Compile a signature. `resolvers` must be non-empty.
Result<Signature> make_signature(std::string_view id, std::string_view section,
                                 std::string_view hex,
                                 std::vector<ResolveSpec> resolvers);

// Resolve resolver `resolver_index` for a match found at `offset` within
// `section`. Returns nullopt when the strategy would read out of the copied
// section bytes.
std::optional<uintptr_t> resolve_match(const Signature& sig,
                                       const SectionCopy& section,
                                       size_t offset,
                                       size_t resolver_index = 0);

}  // namespace hoyoflux::scan
