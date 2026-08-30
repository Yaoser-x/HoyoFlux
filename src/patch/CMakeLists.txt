# Patch engine: remote memory primitives, the in-game RemoteState block and
# the plan applier. Knows *how* to write; the GameAdapter knows *what*.

add_library(hoyoflux_patch STATIC
  memory_writer.cpp
  remote_state.cpp
  patch_engine.cpp
)

target_include_directories(hoyoflux_patch PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(hoyoflux_patch
  PUBLIC hoyoflux_domain hoyoflux_platform_win32 hoyoflux_scan
)
