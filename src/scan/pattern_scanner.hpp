#pragma once

// Anchor search + wildcard verify scanner.
//
// The first byte of the longest fixed run is located with an SSE2
// 16-byte compare sweep; candidates then verify the anchor with memcmp and
// the wildcarded prefix/suffix with the mask. Boundary handling is explicit:
// pattern longer than region is an immediate no-match (the legacy code
// underflowed here), and reads never cross the region end (plan §16).

#include "scan/compiled_pattern.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace hoyoflux::scan {

// All match offsets within [0, region.size() - pattern.length()].
std::vector<size_t> scan_all(const CompiledPattern& pattern,
                             std::span<const std::byte> region);

// First match offset, or nullopt when there is none.
std::optional<size_t> scan_first(const CompiledPattern& pattern,
                                 std::span<const std::byte> region);

}  // namespace hoyoflux::scan
