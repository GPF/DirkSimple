cmake_minimum_required(VERSION 3.12)
project(DirkSimple-Dreamcast C)

# Dreamcast-only build
set(CMAKE_SYSTEM_NAME Dreamcast)

# Optional override for game data base dir
set(DIRKSIMPLE_FORCE_BASE_DIR "" CACHE STRING "Force base dir path")

# Explicitly include KOS library and include paths
link_directories(${KOS_BASE}/lib)
include_directories(${KOS_BASE}/include)

# Main Dreamcast executable
add_executable(dirksimple_dreamcast
    dirksimple_dreamcast.c
)

# Lua scripting support
add_library(dirksimple-lua STATIC
    thirdparty/lua/src/lapi.c thirdparty/lua/src/ldebug.c thirdparty/lua/src/ldo.c
    thirdparty/lua/src/ldump.c thirdparty/lua/src/lfunc.c thirdparty/lua/src/lgc.c
    thirdparty/lua/src/lmem.c thirdparty/lua/src/lobject.c thirdparty/lua/src/lopcodes.c
    thirdparty/lua/src/lstate.c thirdparty/lua/src/lstring.c thirdparty/lua/src/ltable.c
    thirdparty/lua/src/ltm.c thirdparty/lua/src/lundump.c thirdparty/lua/src/lvm.c
    thirdparty/lua/src/lzio.c thirdparty/lua/src/lauxlib.c thirdparty/lua/src/lbaselib.c
    thirdparty/lua/src/lstrlib.c thirdparty/lua/src/ltablib.c thirdparty/lua/src/lctype.c
    thirdparty/lua/src/lparser.c thirdparty/lua/src/llex.c thirdparty/lua/src/lcode.c
    thirdparty/lua/src/linit.c thirdparty/lua/src/ldblib.c thirdparty/lua/src/liolib.c
    thirdparty/lua/src/lmathlib.c thirdparty/lua/src/loslib.c thirdparty/lua/src/lcorolib.c
    thirdparty/lua/src/loadlib.c
)
target_include_directories(dirksimple-lua PRIVATE thirdparty/lua/src)
set_target_properties(dirksimple-lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(dirksimple-lua PRIVATE -DLUA_USE_POSIX=1)

# LodePNG for optional image support
add_library(dirksimple-lodepng STATIC thirdparty/lodepng/lodepng.c)
target_include_directories(dirksimple-lodepng PRIVATE thirdparty/lodepng)
set_target_properties(dirksimple-lodepng PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Include Lua and LodePNG paths
target_include_directories(dirksimple_dreamcast PRIVATE
    thirdparty/lua/src
    thirdparty/lodepng
)

# Link libraries, including Zstandard and fastmem
target_link_libraries(dirksimple_dreamcast
    dirksimple-lua
    dirksimple-lodepng
    m z
    zstd
    fastmem
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
