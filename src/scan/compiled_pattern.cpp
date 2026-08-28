#include "scan/compiled_pattern.hpp"

#include <cctype>
#include <optional>
#include <string>

namespace hoyoflux::scan {
namespace {

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::unexpected<Error> pattern_error(std::string_view why) {
    return std::unexpected(Error::make(ErrorCode::InvalidArgument, std::string(why)));
}

}  // namespace

Result<CompiledPattern> compile_pattern(std::string_view hex) {
    CompiledPattern out;

    size_t i = 0;
    bool saw_any = false;
    while (i < hex.size()) {
        const char c = hex[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '?') {
            // One wildcard byte; swallow an immediately following '?' ("??").
            out.bytes.push_back(std::byte{0});
            out.mask.push_back(false);
            saw_any = true;
            ++i;
            if (i < hex.size() && hex[i] == '?') {
                ++i;
            }
            continue;
        }

        const int hi = hex_nibble(c);
        if (hi < 0) {
            return pattern_error("invalid character in signature");
        }
        ++i;
        if (i >= hex.size() || hex_nibble(hex[i]) < 0) {
            return pattern_error("signature byte must be two hex digits");
        }
        const int lo = hex_nibble(hex[i]);
        ++i;

        out.bytes.push_back(static_cast<std::byte>(hi * 16 + lo));
        out.mask.push_back(true);
        saw_any = true;
    }

    if (!saw_any || out.bytes.empty()) {
        return pattern_error("empty signature");
    }

    // Longest run of fixed bytes == the anchor. All-wildcard patterns cannot
    // be anchored and are rejected.
    size_t best_len = 0;
    size_t best_start = 0;
    size_t run_len = 0;
    size_t run_start = 0;
    for (size_t k = 0; k < out.mask.size(); ++k) {
        if (out.mask[k]) {
            if (run_len == 0) {
                run_start = k;
            }
            ++run_len;
            if (run_len > best_len) {
                best_len = run_len;
                best_start = run_start;
            }
        } else {
            run_len = 0;
        }
    }
    if (best_len == 0) {
        return pattern_error("signature has no fixed bytes");
    }

    out.anchor_index = best_start;
    out.anchor_length = best_len;
    return out;
}

}  // namespace hoyoflux::scan
