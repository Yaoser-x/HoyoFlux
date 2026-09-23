# Win32 platform boundary. Plain Win32 only (no NTSYSAPI / syscall layer).
# Depends on the domain layer for Error/Result; nothing above this may be
# referenced from here.

add_library(hoyoflux_platform_win32 STATIC
  display.cpp
  dialog.cpp
  elevation.cpp
  file.cpp
  notification.cpp
  pe.cpp
  privilege.cpp
  process.cpp
  registry.cpp
)

target_include_directories(hoyoflux_platform_win32 PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(hoyoflux_platform_win32
  PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0600
)

target_link_libraries(hoyoflux_platform_win32
  PUBLIC hoyoflux_domain
  PRIVATE advapi32 bcrypt comctl32 user32 shell32
)
