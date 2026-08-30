# Session orchestration: state machine, journal, recovery. The only module
# allowed to coordinate adapters, patch engine and platform teardown.

add_library(hoyoflux_session STATIC
  display_guard.cpp
  journal.cpp
  persistent_state_guard.cpp
  session_engine.cpp
)

target_include_directories(hoyoflux_session PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(hoyoflux_session
  PUBLIC hoyoflux_domain hoyoflux_game hoyoflux_patch hoyoflux_platform_win32
         hoyoflux_scan hoyoflux_runtime
)

if(HOYOFLUX_EXPERIMENTAL_MOBILE_UI)
  target_compile_definitions(
    hoyoflux_session
    PRIVATE HOYOFLUX_EXPERIMENTAL_MOBILE_UI=1
  )
endif()
