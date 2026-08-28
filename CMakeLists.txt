cmake_minimum_required(VERSION 3.24)
project(HoyoFlux VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(HOYOFLUX_BUILD_TESTS "Build the unit tests" ON)
option(HOYOFLUX_BUILD_BENCHMARKS "Build the benchmarks" OFF)

# ---------------------------------------------------------------------------
# Warnings. Primary toolchain is clang/MinGW; an MSVC path is kept for the
# day the project is built with Visual Studio again.
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
elseif(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus)
endif()

# ---------------------------------------------------------------------------
# Component targets. Dependency direction is strictly one-way:
#   domain -> platform_win32 / scan -> patch -> game -> session -> app -> cli
# Each component declares its own target in its own CMakeLists.txt.
# ---------------------------------------------------------------------------
add_subdirectory(src/domain)

if(HOYOFLUX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(HOYOFLUX_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
