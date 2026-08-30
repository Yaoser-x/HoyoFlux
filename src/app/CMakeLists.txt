# Command-line front end (A10). Everything user-facing lives here; the
# engine below stays quiet.

enable_language(RC)

add_executable(hoyoflux WIN32
  main.cpp
  doctor.cpp
  launch_service.cpp
  hoyoflux.rc
)

target_include_directories(hoyoflux PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(FetchContent)
FetchContent_Declare(
  cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG        v2.4.2
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cli11)

target_link_libraries(hoyoflux PRIVATE
  hoyoflux_session
  hoyoflux_profile
  hoyoflux_platform_win32
  CLI11::CLI11
  shell32
  ole32
)

install(TARGETS hoyoflux RUNTIME DESTINATION bin)
