// Scanner 2.0 benchmark: 16 / 64 / 256 MB regions x 5 / 20 / 50 patterns.
//
// Patterns are planted to NOT match, i.e. every sweep runs the full region
// (the worst case that matters for launch latency). Only if this benchmark
// proves the scanner is a real bottleneck should an AVX2 backend be added;
// AVX512 is out of scope for 1.0.0.

#include <nanobench.h>

#include "scan/compiled_pattern.hpp"
#include "scan/pattern_scanner.hpp"

#include <chrono>
#include <cstdio>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

using namespace hoyoflux;

namespace {

std::string pattern_hex(size_t i) {
    const unsigned a = 0x40 + static_cast<unsigned>((i * 7) % 0x20);
    const unsigned b = 0x90 + static_cast<unsigned>((i * 13) % 0x40);
    const unsigned c = 0x10 + static_cast<unsigned>((i * 29) % 0x80);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02X %02X %02X ?? ?? ?? ?? %02X %02X %02X", a,
                  b, c, b, c, a);
    return buf;
}

}  // namespace

int main() {
    std::printf("HoyoFlux scanner benchmark (no-match sweeps, worst case)\n");
    std::printf("==========================================================\n");

    for (const size_t mb : {16u, 64u, 256u}) {
        const size_t region_bytes = mb * 1024u * 1024u;
        // Scale the chrono run count so the whole benchmark stays quick.
        constexpr int kRuns = 40;
        const int runs = mb <= 16 ? kRuns : (mb <= 64 ? 16 : 5);
        std::vector<std::byte> region(region_bytes, std::byte{0x90});

        for (const size_t count : {5u, 20u, 50u}) {
            std::vector<scan::CompiledPattern> patterns;
            patterns.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                auto pattern = scan::compile_pattern(pattern_hex(i));
                if (!pattern) {
                    std::fprintf(stderr, "compile failed for pattern %zu\n", i);
                    return 2;
                }
                patterns.push_back(std::move(*pattern));
            }

            const double run_bytes = static_cast<double>(region_bytes) * count;

            ankerl::nanobench::Bench bench;
            bench.title("scan " + std::to_string(mb) + " MB x " +
                        std::to_string(count) + " patterns")
                .unit("ns/sweep")
                .warmup(1)
                .minEpochIterations(5);
            bench.run("full sweep", [&] {
                size_t sink = 0;
                for (const auto& pattern : patterns) {
                    auto match = scan::scan_first(pattern, std::span(region));
                    sink += match.value_or(0);
                }
                return sink;
            });

            // Independent chrono timing -> MB/s, unaffected by nanobench's model.
            size_t sink = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int r = 0; r < runs; ++r) {
                for (const auto& pattern : patterns) {
                    auto match = scan::scan_first(pattern, std::span(region));
                    sink += match.value_or(0);
                }
            }
            const auto t1 = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(t1 - t0).count();
            const double gb = run_bytes * runs / 1e9;
            std::printf("  %zu MB x %zu: %.2f GB/s (%.1f MB per full sweep, %d sweeps)\n",
                        mb, count, gb / seconds, run_bytes / 1e6, runs);
            (void)sink;
        }
    }
    return 0;
}
