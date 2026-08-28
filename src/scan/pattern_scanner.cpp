#include "scan/pattern_scanner.hpp"

#include <emmintrin.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace hoyoflux::scan {
namespace {

using byte = unsigned char;

unsigned ctz32(uint32_t v) {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanForward(&index, v);
    return static_cast<unsigned>(index);
#else
    return static_cast<unsigned>(__builtin_ctz(v));
#endif
}

// Push every offset in [begin, end) at which `region[off] == needle`.
// Bulk of the work runs as SSE2 16-byte compares; the tail is scalar. Only
// full 16-byte chunks fully inside [begin, end) are read, so no byte past the
// region is ever touched.
void find_byte_candidates(const byte* region, size_t begin, size_t end, byte needle,
                          std::vector<size_t>& out) {
    const __m128i needle_v = _mm_set1_epi8(static_cast<char>(needle));
    size_t i = begin;
    while (i + 16 <= end) {
        const __m128i chunk = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(region + i));
        uint32_t mask = static_cast<uint32_t>(
            _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, needle_v)));
        while (mask != 0) {
            const unsigned bit = ctz32(mask);
            out.push_back(i + bit);
            mask &= mask - 1;
        }
        i += 16;
    }
    for (; i < end; ++i) {
        if (region[i] == needle) {
            out.push_back(i);
        }
    }
}

// Verify region[pos..pos+count) against pattern[pat..pat+count), honoring
// wildcards.
bool masked_equals(const CompiledPattern& pattern, const byte* region, size_t pos,
                   size_t pat, size_t count) {
    const byte* pattern_bytes = reinterpret_cast<const byte*>(pattern.bytes.data());
    for (size_t i = 0; i < count; ++i) {
        if (pattern.mask[pat + i] && region[pos + i] != pattern_bytes[pat + i]) {
            return false;
        }
    }
    return true;
}

bool is_match_at(const CompiledPattern& pattern, const byte* region, size_t pos) {
    const size_t anchor = pattern.anchor_index;
    const size_t anchor_len = pattern.anchor_length;
    const byte* pattern_bytes = reinterpret_cast<const byte*>(pattern.bytes.data());

    if (std::memcmp(region + pos + anchor, pattern_bytes + anchor, anchor_len) != 0) {
        return false;
    }
    if (anchor > 0 && !masked_equals(pattern, region, pos, 0, anchor)) {
        return false;
    }
    const size_t suffix = anchor + anchor_len;
    if (suffix < pattern.length() &&
        !masked_equals(pattern, region, pos + suffix, suffix, pattern.length() - suffix)) {
        return false;
    }
    return true;
}

}  // namespace

std::vector<size_t> scan_all(const CompiledPattern& pattern,
                             std::span<const std::byte> region) {
    std::vector<size_t> matches;
    const size_t pattern_len = pattern.length();
    const size_t region_len = region.size();
    if (pattern_len == 0 || pattern_len > region_len) {
        return matches;  // no room (fixes the legacy scanEnd underflow)
    }

    const auto* base = reinterpret_cast<const byte*>(region.data());
    const byte* pattern_bytes = reinterpret_cast<const byte*>(pattern.bytes.data());
    const size_t anchor = pattern.anchor_index;
    const size_t last = region_len - pattern_len;

    // Window in which the anchor's first byte can appear: region[pos+anchor]
    // for pos in [0, last]  =>  offsets [anchor, last+anchor+1).
    std::vector<size_t> candidates;
    find_byte_candidates(base, anchor, last + anchor + 1,
                         pattern_bytes[anchor], candidates);
    for (const size_t candidate : candidates) {
        const size_t pos = candidate - anchor;
        if (is_match_at(pattern, base, pos)) {
            matches.push_back(pos);
        }
    }
    return matches;
}

std::optional<size_t> scan_first(const CompiledPattern& pattern,
                                 std::span<const std::byte> region) {
    const size_t pattern_len = pattern.length();
    const size_t region_len = region.size();
    if (pattern_len == 0 || pattern_len > region_len) {
        return std::nullopt;
    }

    const auto* base = reinterpret_cast<const byte*>(region.data());
    const byte* pattern_bytes = reinterpret_cast<const byte*>(pattern.bytes.data());
    const size_t anchor = pattern.anchor_index;
    const size_t last = region_len - pattern_len;

    std::vector<size_t> candidates;
    find_byte_candidates(base, anchor, last + anchor + 1,
                         pattern_bytes[anchor], candidates);
    for (const size_t candidate : candidates) {
        const size_t pos = candidate - anchor;
        if (is_match_at(pattern, base, pos)) {
            return pos;
        }
    }
    return std::nullopt;
}

}  // namespace hoyoflux::scan
