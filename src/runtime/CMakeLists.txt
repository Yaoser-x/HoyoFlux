# Runtime controller (F6/F7): the resident component for in-session
# dynamics - power save foreground reactions and hotkeys. Depends only on
# the domain layer; the session engine hands it an fps writer callback.

add_library(hoyoflux_runtime STATIC
  runtime_controller.cpp
)

target_include_directories(hoyoflux_runtime PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(hoyoflux_runtime
  PUBLIC hoyoflux_domain
  PRIVATE user32
)
