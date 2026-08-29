# Profile store (A9): TOML config parsed into domain::Profile values.
add_library(hoyoflux_profile STATIC
  config.cpp
)

target_include_directories(hoyoflux_profile PUBLIC ${CMAKE_SOURCE_DIR}/src)

include(FetchContent)

# tomlplusplus (header-only TOML parser, v3)
FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG        v3.4.0
  GIT_SHALLOW    TRUE
)
set(TOMLPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tomlplusplus)

target_link_libraries(hoyoflux_profile
  PUBLIC hoyoflux_domain hoyoflux_platform_win32 tomlplusplus::tomlplusplus
)
