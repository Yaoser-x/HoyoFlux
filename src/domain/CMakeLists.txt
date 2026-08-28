# Add a CMake target per component in this directory:
#
#   hoyoflux_domain       ->  src/domain
#   hoyoflux_platform_win32 -> src/platform/win32
#   hoyoflux_scan         ->  src/scan
#   hoyoflux_patch        ->  src/patch
#   hoyoflux_game         ->  src/game
#   hoyoflux_session      ->  src/session
#   hoyoflux_app          ->  src/app
#   hoyoflux_cli          ->  src/frontend
#
# The dependency order in add_subdirectory matters only for discovery; the
# actual edges are expressed in each target's target_link_libraries.
#
# Currently implemented: domain. Remaining components are added as they land.

# The include root is the repository `src/` tree, so headers are referenced
# as `domain/error.hpp`, `platform/win32/process.hpp`, etc. across components.
add_library(hoyoflux_domain INTERFACE)
target_include_directories(hoyoflux_domain INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(hoyoflux_domain INTERFACE cxx_std_23)
