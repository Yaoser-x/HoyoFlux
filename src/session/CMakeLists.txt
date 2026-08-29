# Session orchestration: state machine, journal, recovery. The only module
# allowed to coordinate adapters, patch engine and platform teardown.

add_library(hoyoflux_session STATIC
  journal.cpp
  session_engine.cpp
)

target_include_directories(hoyoflux_session PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(hoyoflux_session
  PUBLIC hoyoflux_domain hoyoflux_game hoyoflux_patch hoyoflux_platform_win32
         hoyoflux_scan
)
