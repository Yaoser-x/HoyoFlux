# Pattern scanning component: compiled patterns, the scanner (A4) and the
# module snapshot (A3). Depends on the platform layer for process access.

add_library(hoyoflux_scan STATIC
  module_snapshot.cpp
  compiled_pattern.cpp
  pattern_scanner.cpp
)

target_include_directories(hoyoflux_scan PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(hoyoflux_scan
  PUBLIC hoyoflux_platform_win32 hoyoflux_domain
)
