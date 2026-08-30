// Signature table tests: every ported Genshin/StarRail signature compiles,
// scans a synthetic buffer, and resolves with the exact legacy arithmetic.

#include "game/genshin/signatures.hpp"
#include "game/starrail/signatures.hpp"
#include "scan/pattern_scanner.hpp"
#include "scan/signature.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

using namespace hoyoflux;
namespace genshin = hoyoflux::game::genshin;
namespace starrail = hoyoflux::game::starrail;
namespace scan = hoyoflux::scan;

namespace {

// Build a SectionCopy holding the signature's pattern at `lead` (default 0),
// with the 32-bit value at disp_offset overwritten. The pattern's fixed bytes
// are copied from the compiled signature itself, so the test never needs the
// raw hex strings.
scan::SectionCopy make_section_with_pattern(const scan::Signature& sig,
                                            int64_t disp_offset, int32_t disp_value,
                                            size_t lead = 0) {
    scan::SectionCopy sec;
    sec.name = ".text";
    sec.rva = 0x1000;
    sec.remote_address = 0x140000000 + sec.rva;

    const size_t pattern_len = sig.pattern.length();
    const size_t size =
        lead + std::max(pattern_len, static_cast<size_t>(disp_offset) + 8) + 16;
    sec.bytes.assign(size, std::byte{0xCC});

    for (size_t i = 0; i < pattern_len; ++i) {
        if (sig.pattern.mask[i]) {
            sec.bytes[lead + i] = sig.pattern.bytes[i];
        }
    }
    if (disp_offset >= 0) {
        std::memcpy(sec.bytes.data() + lead + disp_offset, &disp_value, sizeof(int32_t));
    }
    return sec;
}

}  // namespace

TEST_CASE("every genshin signature compiles and scans itself", "[game][genshin]") {
    for (const auto id : genshin::all_ids()) {
        auto sig = genshin::signature(id);
        REQUIRE(sig.has_value());
        CHECK_FALSE(sig->pattern.length() == 0);
        CHECK_FALSE(sig->resolvers.empty());
        CHECK_FALSE(sig->section.empty());

        // No disp overwrite: this test only proves the pattern matches itself.
        const auto sec = make_section_with_pattern(*sig, -1, 0);
        auto match = scan::scan_first(sig->pattern, sec.bytes);
        REQUIRE(match.has_value());
        CHECK(*match == 0);
    }
}

TEST_CASE("every starrail signature compiles and scans itself", "[game][starrail]") {
    for (const auto id : starrail::all_ids()) {
        auto sig = starrail::signature(id);
        REQUIRE(sig.has_value());
        const auto sec = make_section_with_pattern(*sig, -1, 0);
        auto match = scan::scan_first(sig->pattern, sec.bytes);
        REQUIRE(match.has_value());
        CHECK(*match == 0);
    }
}

TEST_CASE("genshin Fps55 resolves to the disp field address", "[game][genshin]") {
    auto sig = genshin::signature(genshin::SignatureId::Fps55);
    REQUIRE(sig.has_value());
    const auto sec = make_section_with_pattern(*sig, 4, 0x1234);
    auto resolved = scan::resolve_match(*sig, sec, 0);
    REQUIRE(resolved.has_value());
    // legacy: pfps = addr + 4
    CHECK(*resolved == sec.remote_address + 4);
}

TEST_CASE("genshin Fps54 resolves with call skip arithmetic", "[game][genshin]") {
    constexpr int32_t kDisp = 0x1234;
    auto sig = genshin::signature(genshin::SignatureId::Fps54);
    REQUIRE(sig.has_value());
    const auto sec = make_section_with_pattern(*sig, 3, kDisp);
    auto resolved = scan::resolve_match(*sig, sec, 0);
    REQUIRE(resolved.has_value());
    // legacy: rip = addr+3; rip += *(i32)(rip) + 6
    CHECK(*resolved == sec.remote_address + 3 + 6 + kDisp);
}

TEST_CASE("starrail Fps resolves with standard rip-relative math", "[game][starrail]") {
    constexpr int32_t kDisp = -0x40;
    auto sig = starrail::signature(starrail::SignatureId::Fps);
    REQUIRE(sig.has_value());
    const auto sec = make_section_with_pattern(*sig, 4, kDisp);
    auto resolved = scan::resolve_match(*sig, sec, 0);
    REQUIRE(resolved.has_value());
    // legacy: rip = addr+4; rip += *(i32)(rip) + 4
    const int64_t expected = static_cast<int64_t>(sec.remote_address) + 4 + 4 + kDisp;
    REQUIRE(expected >= 0);
    CHECK(*resolved == static_cast<uintptr_t>(expected));
}

TEST_CASE("starrail UISet variants use skip=8", "[game][starrail]") {
    constexpr int32_t kDisp = 0x10;
    const int64_t offsets[] = {15, 11, 9};
    const starrail::SignatureId ids[] = {
        starrail::SignatureId::UISetV1,
        starrail::SignatureId::UISetV2,
        starrail::SignatureId::UISetV3,
    };
    for (size_t i = 0; i < 3; ++i) {
        auto sig = starrail::signature(ids[i]);
        REQUIRE(sig.has_value());
        const auto sec = make_section_with_pattern(*sig, offsets[i], kDisp);
        auto resolved = scan::resolve_match(*sig, sec, 0);
        REQUIRE(resolved.has_value());
        CHECK(*resolved == sec.remote_address + offsets[i] + 8 + kDisp);
    }
}

TEST_CASE("genshin mobile UI v1 has four resolvers", "[game][genshin]") {
    auto sig = genshin::signature(genshin::SignatureId::MobileUiV1);
    REQUIRE(sig.has_value());
    REQUIRE(sig->resolvers.size() == 4);
    // Build a section with all four offset fields populated.
    const int32_t values[] = {0x111, 0x222, 0x333, 0x444};
    const int64_t offsets[] = {3, 0xA, 0x20, 0x30};
    scan::SectionCopy sec;
    sec.remote_address = 0x140000000;
    const size_t pattern_len = sig->pattern.length();
    const size_t size = std::max(pattern_len, static_cast<size_t>(0x34)) + 16;
    sec.bytes.assign(size, std::byte{0xCC});
    for (size_t i = 0; i < pattern_len; ++i) {
        if (sig->pattern.mask[i]) {
            sec.bytes[i] = sig->pattern.bytes[i];
        }
    }
    for (size_t i = 0; i < 4; ++i) {
        std::memcpy(sec.bytes.data() + offsets[i], &values[i], sizeof(int32_t));
    }
    // resolver 0: Grph_class = addr+3+4+disp ; resolver 1: raw i32 @0xA
    auto grph_class = scan::resolve_match(*sig, sec, 0, 0);
    REQUIRE(grph_class.has_value());
    CHECK(*grph_class == sec.remote_address + 3 + 4 + values[0]);
    auto ui_cl = scan::resolve_match(*sig, sec, 0, 1);
    REQUIRE(ui_cl.has_value());
    CHECK(*ui_cl == static_cast<uintptr_t>(static_cast<uint32_t>(values[1])));
    auto gui_set = scan::resolve_match(*sig, sec, 0, 2);
    REQUIRE(gui_set.has_value());
    CHECK(*gui_set == sec.remote_address + 0x20 + 4 + values[2]);
    auto input_set = scan::resolve_match(*sig, sec, 0, 3);
    REQUIRE(input_set.has_value());
    CHECK(*input_set == sec.remote_address + 0x30 + 4 + values[3]);
}

TEST_CASE("genshin unhooktime exposes lifecycle call and original callee",
          "[game][genshin]") {
    auto sig = genshin::signature(genshin::SignatureId::UnhookTime);
    REQUIRE(sig.has_value());
    REQUIRE(sig->resolvers.size() == 2);
    CHECK(sig->resolvers[0].strategy == scan::ResolveStrategy::FieldDisp);
    CHECK(sig->resolvers[1].strategy == scan::ResolveStrategy::RipRelative);
}

TEST_CASE("direct signatures resolve to the match itself", "[game][genshin]") {
    auto sig = genshin::signature(genshin::SignatureId::PayloadOep);
    REQUIRE(sig.has_value());
    const auto sec = make_section_with_pattern(*sig, -1, 0);
    auto resolved = scan::resolve_match(*sig, sec, 0);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == sec.remote_address);
}

TEST_CASE("resolve reads past the section return nullopt", "[scan][signature]") {
    auto sig = genshin::signature(genshin::SignatureId::Fps54);
    REQUIRE(sig.has_value());
    // Truncated copy: pattern present but the disp field is cut short.
    scan::SectionCopy sec;
    sec.remote_address = 0x1000;
    sec.bytes.assign(sig->pattern.length() - 1, std::byte{0xCC});
    auto match = scan::scan_first(sig->pattern, sec.bytes);
    CHECK_FALSE(match.has_value());  // pattern > region
    CHECK_FALSE(scan::resolve_match(*sig, sec, 0).has_value());
}

TEST_CASE("match at a non-zero offset resolves against remote base", "[scan][signature]") {
    auto sig = genshin::signature(genshin::SignatureId::FpsOld);
    REQUIRE(sig.has_value());
    const auto sec = make_section_with_pattern(*sig, 4, 0x777, /*lead=*/64);
    auto match = scan::scan_first(sig->pattern, sec.bytes);
    REQUIRE(match.has_value());
    CHECK(*match == 64);
    auto resolved = scan::resolve_match(*sig, sec, 64);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == sec.remote_address + 64 + 4);
}
