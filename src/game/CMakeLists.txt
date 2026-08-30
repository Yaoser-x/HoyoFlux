# Game knowledge: per-game adapters (A5) and the data-ified signature tables
# (A4). Each game is self-contained: a game update only touches its own
# signatures.hpp / adapter.cpp.

add_library(hoyoflux_game STATIC
  adapter.cpp
  launch/launch_plan.cpp
  genshin/signatures.cpp
  genshin/genshin_adapter.cpp
  genshin/mobile_ui.cpp
  starrail/signatures.cpp
  starrail/starrail_adapter.cpp
  starrail/mobile_ui.cpp
)

target_include_directories(hoyoflux_game PUBLIC ${CMAKE_SOURCE_DIR}/src)

if(HOYOFLUX_EXPERIMENTAL_MOBILE_UI)
  target_compile_definitions(
    hoyoflux_game
    PRIVATE HOYOFLUX_EXPERIMENTAL_MOBILE_UI=1
  )
endif()

target_link_libraries(hoyoflux_game
  PUBLIC hoyoflux_scan hoyoflux_platform_win32 hoyoflux_domain
)
