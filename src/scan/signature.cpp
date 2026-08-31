#include "scan/signature.hpp"

#include <cstring>
#include <string>

namespace hoyoflux::scan {

Result<Signature> make_signature(std::string_view id, std::string_view section,
                                 std::string_view hex,
                                 std::vector<ResolveSpec> resolvers) {
    if (resolvers.empty()) {
        return std::unexpected(Error::make(ErrorCode::InvalidArgument,
                                           "signature needs at least one resolver"));
    }
    auto pattern = compile_pattern(hex);
    if (!pattern) {
        return std::unexpected(pattern.error());
    }
    Signature signature;
    signature.id = id;
    signature.section = section;
    signature.pattern = std::move(*pattern);
    signature.resolvers = std::move(resolvers);
    return signature;
}

std::optional<uintptr_t> resolve_match(const Signature& sig,
                                       const SectionCopy& section,
                                       size_t offset, size_t resolver_index) {
    if (resolver_index >= sig.resolvers.size()) {
        return std::nullopt;
    }
    const ResolveSpec& spec = sig.resolvers[resolver_index];
    const uintptr_t found = section.remote_address + offset;

    switch (spec.strategy) {
    case ResolveStrategy::Direct:
        return found;

    case ResolveStrategy::FieldDisp:
        return found + static_cast<uintptr_t>(spec.disp_offset);

    case ResolveStrategy::RipRelative: {
        const size_t field = offset + static_cast<size_t>(spec.disp_offset);
        if (field + 4 > section.bytes.size()) {
            return std::nullopt;  // would read past the copied section
        }
        int32_t disp = 0;
        std::memcpy(&disp, section.bytes.data() + field, sizeof(disp));
        const int64_t resolved =
            static_cast<int64_t>(found) + spec.disp_offset + spec.skip + disp;
        if (resolved < 0) {
            return std::nullopt;
        }
        return static_cast<uintptr_t>(resolved);
    }

    case ResolveStrategy::RawInt32At: {
        const size_t field = offset + static_cast<size_t>(spec.disp_offset);
        if (field + 4 > section.bytes.size()) {
            return std::nullopt;
        }
        int32_t value = 0;
        std::memcpy(&value, section.bytes.data() + field, sizeof(value));
        return static_cast<uintptr_t>(static_cast<uint32_t>(value));
    }
    }
    return std::nullopt;
}

}  // namespace hoyoflux::scan
