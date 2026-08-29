# Game knowledge: per-game adapters (A5) and the data-ified signature tables
# (A4). Each game is self-contained: a game update only touches its own
# signatures.hpp / adapter.cpp.

add_library(hoyoflux_game STATIC
  genshin/signatures.cpp
  starrail/signatures.cpp
)

target_include_directories(hoyoflux_game PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(hoyoflux_game
  PUBLIC hoyoflux_scan hoyoflux_platform_win32 hoyoflux_domain
)
