# Portable double-click front end. Configuration and runtime state live beside
# the executable; no public command-line surface is linked into the product.

enable_language(RC)

add_executable(hoyoflux WIN32
  main.cpp
  app_paths.cpp
  doctor.cpp
  elevated_session.cpp
  launch_service.cpp
  portable_migration.cpp
  hoyoflux.rc
  ${CMAKE_BINARY_DIR}/generated/hoyoflux-version.rc
)

if(MSVC)
  # hoyoflux.rc embeds the one authoritative asInvoker manifest. Do not let
  # the MSVC linker generate a second manifest resource.
  target_link_options(hoyoflux PRIVATE /MANIFEST:NO)
endif()

if(MINGW)
  # Release artifacts are a single portable EXE. The supported llvm-mingw
  # toolchain ships static libc++/libunwind archives for this purpose.
  target_link_options(hoyoflux PRIVATE $<$<CONFIG:Release>:-static>)
endif()

target_include_directories(hoyoflux PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(hoyoflux PRIVATE
  hoyoflux_session
  hoyoflux_profile
  hoyoflux_platform_win32
  shell32
  ole32
)

install(TARGETS hoyoflux RUNTIME DESTINATION bin)
