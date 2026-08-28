# Benchmarks. Built with the release preset (HOYOFLUX_BUILD_BENCHMARKS=ON).

include(FetchContent)

FetchContent_Declare(
  nanobench
  GIT_REPOSITORY https://github.com/martinus/nanobench.git
  GIT_TAG        v4.6.0
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(nanobench)

add_executable(bench_pattern_scanner bench_pattern_scanner.cpp)
target_link_libraries(bench_pattern_scanner PRIVATE hoyoflux_domain hoyoflux_scan
                      nanobench::nanobench)
