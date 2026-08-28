// Scanner 2.0 unit tests: correctness, wildcards and every boundary case the
// legacy scanner got wrong (pattern > region, tail matches, all-wildcard).

#include "scan/compiled_pattern.hpp"
#include "scan/pattern_scanner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <vector>

using namespace hoyoflux;
namespace scan = hoyoflux::scan;

namespace {

std::vector<std::byte> buf(std::initializer_list<unsigned> vals) {
    std::vector<std::byte> out;
    out.reserve(vals.size());
    for (unsigned v : vals) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// The signature family used across these tests: 3 fixed bytes, 4 wildcards,
// 3 fixed bytes. Wildcards are "48 8B 05 ?? ?? ?? ?? 48 8B 88".
scan::CompiledPattern fps_pattern() {
    auto p = scan::compile_pattern("48 8B 05 ?? ?? ?? ?? 48 8B 88");
    REQUIRE(p.has_value());
    return *p;
}

}  // namespace

TEST_CASE("compile: structure and anchor", "[scan][pattern]") {
    auto p = scan::compile_pattern("48 8B 05 ?? ?? ?? ?? 48 8B 88");
    REQUIRE(p.has_value());
    CHECK(p->length() == 10);
    CHECK(p->mask[0]);
    CHECK(p->mask[1]);
    CHECK(p->mask[2]);
    CHECK_FALSE(p->mask[3]);
    CHECK_FALSE(p->mask[6]);
    CHECK(p->mask[7]);
    CHECK(p->mask[8]);
    // Longest fixed run: [0,3) and [7,9) -> length 3.
    CHECK(p->anchor_length == 3);
    CHECK(p->anchor_index == 0);
}

TEST_CASE("compile: consecutive wildcards and ?? form", "[scan][pattern]") {
    auto p = scan::compile_pattern("48 ?? ?? 8B");
    REQUIRE(p.has_value());
    CHECK(p->length() == 4);
    CHECK(p->mask[0]);
    CHECK_FALSE(p->mask[1]);
    CHECK_FALSE(p->mask[2]);
    CHECK(p->mask[3]);
}

TEST_CASE("compile: rejects invalid input", "[scan][pattern]") {
    CHECK_FALSE(scan::compile_pattern("").has_value());
    CHECK_FALSE(scan::compile_pattern("?? ?? ??").has_value());  // all wildcard
    CHECK_FALSE(scan::compile_pattern("zz").has_value());        // not hex
    CHECK_FALSE(scan::compile_pattern("48 8").has_value());      // single nibble
    CHECK_FALSE(scan::compile_pattern("48 8B zz").has_value());
}

TEST_CASE("scanner: exact match with wildcards", "[scan][pattern]") {
    auto pat = fps_pattern();
    std::vector<std::byte> region;
    region.insert(region.end(), 10, std::byte{0xCC});
    region.push_back(std::byte{0x48});
    region.push_back(std::byte{0x8B});
    region.push_back(std::byte{0x05});
    region.push_back(std::byte{0x11});
    region.push_back(std::byte{0x22});
    region.push_back(std::byte{0x33});
    region.push_back(std::byte{0x44});
    region.push_back(std::byte{0x48});
    region.push_back(std::byte{0x8B});
    region.push_back(std::byte{0x88});

    auto found = scan::scan_first(pat, std::span(region));
    REQUIRE(found.has_value());
    CHECK(*found == 10);
}

TEST_CASE("scanner: match at offset 0", "[scan][pattern]") {
    auto pat = fps_pattern();
    auto region = buf({0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x88});
    auto found = scan::scan_first(pat, std::span(region));
    REQUIRE(found.has_value());
    CHECK(*found == 0);
}

TEST_CASE("scanner: match at very end of buffer", "[scan][pattern]") {
    auto pat = fps_pattern();
    std::vector<std::byte> region;
    region.insert(region.end(), 1000, std::byte{0xCC});
    // plant the pattern exactly at the last possible offset
    const size_t pos = region.size() - pat.length();
    region[pos] = std::byte{0x48};
    region[pos + 1] = std::byte{0x8B};
    region[pos + 2] = std::byte{0x05};
    region[pos + 7] = std::byte{0x48};
    region[pos + 8] = std::byte{0x8B};
    region[pos + 9] = std::byte{0x88};

    auto found = scan::scan_first(pat, std::span(region));
    REQUIRE(found.has_value());
    CHECK(*found == pos);
}

TEST_CASE("scanner: no match", "[scan][pattern]") {
    auto pat = fps_pattern();
    std::vector<std::byte> region(256, std::byte{0xCC});
    CHECK_FALSE(scan::scan_first(pat, std::span(region)).has_value());
    CHECK(scan::scan_all(pat, std::span(region)).empty());
}

TEST_CASE("scanner: multiple matches reported in order", "[scan][pattern]") {
    auto pat = scan::compile_pattern("DE AD BE EF");
    REQUIRE(pat.has_value());
    std::vector<std::byte> region = buf({0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xDE, 0xAD,
                                         0xBE, 0xEF, 0x00, 0xDE, 0xAD, 0xBE, 0xEF});
    auto all = scan::scan_all(*pat, std::span(region));
    REQUIRE(all.size() == 3);
    CHECK(all[0] == 0);
    CHECK(all[1] == 5);
    CHECK(all[2] == 10);
    auto first = scan::scan_first(*pat, std::span(region));
    REQUIRE(first.has_value());
    CHECK(*first == 0);
}

TEST_CASE("scanner: wildcards match differing bytes", "[scan][pattern]") {
    auto pat = scan::compile_pattern("48 ?? ?? 8B");
    REQUIRE(pat.has_value());
    // "48 01 02 8B" matches, "48 01 8B 8B" (fixed byte wrong at 2? no, 2 is wild) ...
    auto region = buf({0x48, 0x01, 0x02, 0x8B});
    auto found = scan::scan_first(*pat, std::span(region));
    REQUIRE(found.has_value());
    CHECK(*found == 0);
}

TEST_CASE("scanner: pattern longer than region is no-match", "[scan][pattern]") {
    auto pat = scan::compile_pattern("48 8B 05 ?? ?? ?? ?? 48 8B 88 10 20 30");
    REQUIRE(pat.has_value());
    CHECK(pat->length() == 13);
    auto region = buf({0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x88});
    CHECK_FALSE(scan::scan_first(*pat, std::span(region)).has_value());
    CHECK(scan::scan_all(*pat, std::span(region)).empty());
}

TEST_CASE("scanner: short and empty buffers", "[scan][pattern]") {
    auto pat = fps_pattern();
    std::vector<std::byte> empty;
    CHECK_FALSE(scan::scan_first(pat, std::span(empty)).has_value());
    auto tiny = buf({0x48});
    CHECK_FALSE(scan::scan_first(pat, std::span(tiny)).has_value());
}

TEST_CASE("scanner: large buffer with planted match at end", "[scan][pattern]") {
    auto pat = fps_pattern();
    constexpr size_t kSize = 1024u * 1024u;
    std::vector<std::byte> region(kSize, std::byte{0x90});
    const size_t pos = kSize - pat.length();
    region[pos] = std::byte{0x48};
    region[pos + 1] = std::byte{0x8B};
    region[pos + 2] = std::byte{0x05};
    region[pos + 7] = std::byte{0x48};
    region[pos + 8] = std::byte{0x8B};
    region[pos + 9] = std::byte{0x88};

    auto found = scan::scan_first(pat, std::span(region));
    REQUIRE(found.has_value());
    CHECK(*found == pos);
    auto all = scan::scan_all(pat, std::span(region));
    REQUIRE(all.size() == 1);
    CHECK(all[0] == pos);
}

TEST_CASE("scanner: no false positive when only anchor bytes match", "[scan][pattern]") {
    auto pat = fps_pattern();
    // "48 8B 05" present but the trailing "48 8B 88" is wrong.
    auto region = buf({0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x99});
    CHECK_FALSE(scan::scan_first(pat, std::span(region)).has_value());
}
