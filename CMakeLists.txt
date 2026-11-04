# psprecomp — a static-recompilation toolkit for PlayStation Portable games.
#
# This file builds two things:
#   1. the `psprecomp` runtime static library — what recompiled game code links
#      against (CPU state, memory map, semantic helpers)
#   2. `allegrexrecomp` — the tool that turns a PSP module into C
#
# A game repo consumes this repo as a git submodule and does:
#     add_subdirectory(psprecomp)
#     add_subdirectory(games/<game>)
# and links its host against the `psprecomp` target.
#
# The core has no external dependencies on purpose. SDL2 and friends only ever
# appear behind an opt-in option, so a fresh clone builds with nothing installed.

cmake_minimum_required(VERSION 3.16)
project(psprecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3)
    # /MP: the generated corpus is hundreds of translation units, and building
    # them one at a time is the slowest part of the loop. No /bigobj -- the
    # emitter splits its output, so no single unit is large enough to need it.
    add_compile_options(/MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# --- the runtime library ------------------------------------------------------

add_library(psprecomp STATIC
    src/cpu.c
    src/mem.c
    src/dispatch.c
    src/ctors.c
    src/render.c
    src/vfpu.c
    src/traps.c
    src/window.c
    src/module.c
    src/hle/hle.c
    src/hle/sysmem.c
    src/hle/threadman.c
    src/hle/power.c
    src/hle/intrman.c
    src/hle/display.c
    src/hle/ge.c
    src/hle/ge_thread.c
    src/hle/sascore.c
    src/hle/iofilemgr.c
    src/hle/misc.c
    src/hle/sync.c
    src/hle/utility.c
)

target_include_directories(psprecomp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Runtime VFPU helpers use libm on Unix; propagate it to static-library users.
if(UNIX)
    target_link_libraries(psprecomp PUBLIC m)
endif()

# Real scheduling is experimental; the existing inline model stays the default.
option(PSPRECOMP_REAL_THREADS "Enable experimental cooperative host-thread scheduling" OFF)
target_compile_definitions(psprecomp PUBLIC
    PSPRECOMP_REAL_THREADS=$<BOOL:${PSPRECOMP_REAL_THREADS}>
)
if(WIN32)
    target_link_libraries(psprecomp PUBLIC xinput9_1_0)   # the window's controller input
endif()
if(PSPRECOMP_REAL_THREADS AND WIN32)
    target_link_libraries(psprecomp PUBLIC winmm)   # timeBeginPeriod
endif()
if(PSPRECOMP_REAL_THREADS AND NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(psprecomp PUBLIC Threads::Threads)
endif()

# --- the recompiler -----------------------------------------------------------

add_subdirectory(tools/allegrexrecomp)

# --- tests (only when psprecomp is the top-level project, not a submodule) -----

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests)
endif()
