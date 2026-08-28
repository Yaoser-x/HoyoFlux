#pragma once

// A signature compiled once, then scanned many times.
//
// The legacy scanner re-parsed hex strings and re-allocated working buffers on
// every call (and had a stack-buffer overflow path for long patterns). Here a
// signature is compiled once into bytes + mask + a longest-fixed-run anchor;
// scanning then never parses text or allocates pattern storage (plan §16).

#include "domain/error.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace hoyoflux::scan {

struct CompiledPattern {
    std::vector<std::byte> bytes;  // wildcard positions are zeroed
    std::vector<bool> mask;        // true == byte must match
    size_t anchor_index{0};        // start of the longest fixed (non-wildcard) run
    size_t anchor_length{0};       // 0 only for an all-wildcard pattern (rejected)

    [[nodiscard]] size_t length() const noexcept { return bytes.size(); }
};

// Compile a signature written as space-separated hex with '?' wildcards,
// e.g. "48 8B 05 ?? ?? ?? ?? 48 8B 88". A '?' (or "??") is one wildcard byte.
// Empty patterns and all-wildcard patterns are rejected.
Result<CompiledPattern> compile_pattern(std::string_view hex);

}  // namespace hoyoflux::scan
