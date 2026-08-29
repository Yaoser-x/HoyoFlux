# Unit tests for all components. Each test executable is registered with
# add_test() so `ctest --preset <name>` runs everything.

include(FetchContent)

# Catch2 v3 (test framework, built from source; header dependency of tests only)
FetchContent_Declare(
  catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.9.1
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(catch2)

add_executable(domain_test domain_test.cpp)
target_link_libraries(domain_test PRIVATE hoyoflux_domain Catch2::Catch2WithMain)
add_test(NAME domain_test COMMAND domain_test)

add_executable(win32_test win32_test.cpp)
target_link_libraries(win32_test PRIVATE hoyoflux_domain hoyoflux_platform_win32
                      shell32 Catch2::Catch2WithMain)
add_test(NAME win32_test COMMAND win32_test)

add_executable(scan_test scan_test.cpp)
target_link_libraries(scan_test PRIVATE hoyoflux_domain hoyoflux_platform_win32
                      hoyoflux_scan Catch2::Catch2WithMain)
add_test(NAME scan_test COMMAND scan_test)

add_executable(scan_pattern_test scan_pattern_test.cpp)
target_link_libraries(scan_pattern_test PRIVATE hoyoflux_domain hoyoflux_scan
                      Catch2::Catch2WithMain)
add_test(NAME scan_pattern_test COMMAND scan_pattern_test)

add_executable(signature_test signature_test.cpp)
target_link_libraries(signature_test PRIVATE hoyoflux_domain hoyoflux_scan
                      hoyoflux_game Catch2::Catch2WithMain)
add_test(NAME signature_test COMMAND signature_test)

add_executable(game_adapter_test game_adapter_test.cpp)
target_link_libraries(game_adapter_test PRIVATE hoyoflux_domain hoyoflux_scan
                      hoyoflux_game Catch2::Catch2WithMain)
add_test(NAME game_adapter_test COMMAND game_adapter_test)

add_executable(patch_test patch_test.cpp)
target_link_libraries(patch_test PRIVATE hoyoflux_domain hoyoflux_platform_win32
                      hoyoflux_patch Catch2::Catch2WithMain)
add_test(NAME patch_test COMMAND patch_test)

add_executable(session_test session_test.cpp)
target_link_libraries(session_test PRIVATE hoyoflux_domain hoyoflux_platform_win32
                      hoyoflux_scan hoyoflux_patch hoyoflux_game hoyoflux_session
                      Catch2::Catch2WithMain)
add_test(NAME session_test COMMAND session_test)

add_executable(runtime_test runtime_test.cpp)
target_link_libraries(runtime_test PRIVATE hoyoflux_domain hoyoflux_runtime
                      Catch2::Catch2WithMain)
add_test(NAME runtime_test COMMAND runtime_test)

add_executable(profile_test profile_test.cpp)
target_link_libraries(profile_test PRIVATE hoyoflux_domain hoyoflux_platform_win32
                      hoyoflux_profile Catch2::Catch2WithMain)
add_test(NAME profile_test COMMAND profile_test)
