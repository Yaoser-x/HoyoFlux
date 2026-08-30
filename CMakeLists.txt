cmake_minimum_required(VERSION 3.24)
project(HoyoFlux VERSION 1.0.0 LANGUAGES CXX)

# Version single source (plan §1): CMake generates version.hpp; code reads
# HOYOFLUX_VERSION_STRING from it. Never declare the tree a final release
# while HOYOFLUX_PRERELEASE is set.
set(HOYOFLUX_PRERELEASE "alpha.1")
configure_file(cmake/version.hpp.in ${CMAKE_BINARY_DIR}/generated/version.hpp
               @ONLY)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(HOYOFLUX_BUILD_TESTS "Build the unit tests" ON)
option(HOYOFLUX_BUILD_BENCHMARKS "Build the benchmarks" OFF)
option(
  HOYOFLUX_EXPERIMENTAL_MOBILE_UI
  "Enable Genshin Mobile UI B1 telemetry instrumentation"
  OFF
)

# ---------------------------------------------------------------------------
# Warnings. Primary toolchain is clang/MinGW; an MSVC path is kept for the
# day the project is built with Visual Studio again.
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Note: -Wpedantic is intentionally omitted; Catch2's test registration
  # uses __COUNTER__ which triggers -Wc2y-extensions under -Wpedantic.
  add_compile_options(-Wall -Wextra)
elseif(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus)
endif()

# ---------------------------------------------------------------------------
# Component targets. Dependency direction is strictly one-way:
#   domain -> platform_win32 / scan -> patch -> game -> session ->
#   runtime -> app -> cli
# Each component declares its own target in its own CMakeLists.txt.
# ---------------------------------------------------------------------------
add_subdirectory(src/domain)
add_subdirectory(src/platform/win32)
add_subdirectory(src/scan)
add_subdirectory(src/patch)
add_subdirectory(src/game)
add_subdirectory(src/session)
add_subdirectory(src/runtime)
add_subdirectory(src/profile)
add_subdirectory(src/app)

if(HOYOFLUX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(HOYOFLUX_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
