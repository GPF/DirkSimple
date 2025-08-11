cmake_minimum_required(VERSION 3.12)
project(DirkSimple-Dreamcast C)

set(CMAKE_SYSTEM_NAME Dreamcast)

# Optional override for game data base dir
set(DIRKSIMPLE_FORCE_BASE_DIR "" CACHE STRING "Force base dir path")

# Main Dreamcast executable
add_executable(dirksimple_dreamcast
    dirksimple_dreamcast.c
)

# Rely on kos-ports Lua and libpng (no thirdparty sources!)
target_link_libraries(dirksimple_dreamcast
    lua
    png
    lz4
    fastmem
    m
    z
)

# Compile-time override for base path (optional)
if(NOT DIRKSIMPLE_FORCE_BASE_DIR STREQUAL "")
    target_compile_definitions(dirksimple_dreamcast PRIVATE "DIRKSIMPLE_FORCE_BASE_DIR=${DIRKSIMPLE_FORCE_BASE_DIR}")
endif()

# Post-build: link to data directory for emulator/test runs
add_custom_command(
    TARGET dirksimple_dreamcast POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
    "${CMAKE_CURRENT_SOURCE_DIR}/data"
    "${CMAKE_CURRENT_BINARY_DIR}/data"
)
