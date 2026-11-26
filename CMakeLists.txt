cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Auto Modellista - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(automodellista_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
# Clang: keep the optimizer to MSVC's assumptions for the whole build (the
# runtime also reads guest memory through casts). See "Clang (clang-cl)" below.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    if(MSVC)
        add_compile_options(/clang:-fno-strict-aliasing /clang:-fwrapv)
        # Silence the CRT's "getenv/fopen is unsafe" notices (hundreds of lines).
        add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    else()
        add_compile_options(-fno-strict-aliasing -fwrapv)
    endif()
endif()
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/am_overrides.c
    src/d3d11_backend.c
    src/am_profiler.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dxguid
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )

# ── Auto Modellista ─────────────────────────────────────────────
# Where your extracted disc lives (default.xbe + media/ + src/).
set(AM_GAME_DIR "C:/AMRecomp/Source Files/Unpacked ISO" CACHE PATH
    "Directory containing the extracted Auto Modellista disc")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    AM_GAME_DIR="${AM_GAME_DIR}"
)

# ── Build speed for the generated code ─────────────────────────
# /MP: compile the generated files in parallel instead of one at a time.
#
# AM_OPTIMIZE_GEN (default ON): compile generated code with the normal
# Release optimizations, except the files listed in AM_GEN_NOOPT_FILES.
# Those hold the 7,000+ line functions (sub_001E0E10, sub_001E4870, ...)
# that MSVC's optimizer stalled on for 25+ minutes, so they stay at /Od.
# If a build sits on one generated file for more than ~5 minutes, add it
# to the list:  cmake -S . -B build -DAM_GEN_NOOPT_FILES="recomp_0012.c;recomp_0009.c"
# Set AM_OPTIMIZE_GEN=OFF for a fast, easily debugged /Od build of everything.
option(AM_OPTIMIZE_GEN "Optimize generated recomp code (except AM_GEN_NOOPT_FILES)" ON)
set(AM_GEN_NOOPT_FILES "recomp_0012.c" CACHE STRING
    "Generated files compiled without optimization (semicolon-separated)")
# Flags for those files. /Od is the safe default; "/O1;/Ob0" (optimize for
# size, no inlining) is lighter on the optimizer and worth trying, since the
# slow file seems to hold the audio decoder.
set(AM_GEN_NOOPT_FLAGS "/Od;/Ob0" CACHE STRING
    "Compile options for AM_GEN_NOOPT_FILES (semicolon-separated)")
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /MP)
    foreach(gen_src ${RECOMP_GEN_SOURCES})
        get_filename_component(gen_name ${gen_src} NAME)
        if(NOT AM_OPTIMIZE_GEN)
            set_source_files_properties(${gen_src} PROPERTIES
                COMPILE_OPTIONS "/Od;/Ob0")
        elseif(gen_name IN_LIST AM_GEN_NOOPT_FILES)
            set_source_files_properties(${gen_src} PROPERTIES
                COMPILE_OPTIONS "${AM_GEN_NOOPT_FLAGS}")
        endif()
    endforeach()
endif()

# ── Clang (clang-cl) ────────────────────────────────────────────
# cmake -S . -B build-clang -G "Visual Studio 18 2026" -A x64 -T ClangCL
# The generated code is translated x86: it reinterprets guest memory through
# casts (MEM32 over byte arrays) and relies on two's-complement wrap-around,
# which MSVC never optimizes against but clang does. These two flags keep
# clang's optimizer to MSVC's assumptions; without them, clang may legally
# "optimize" translated code into different behaviour.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE
        # The generated code trips a few warnings by the thousand; they are
        # noise for translated code.
        -Wno-unused-variable -Wno-unused-label -Wno-unused-but-set-variable
        -Wno-parentheses-equality -Wno-self-assign -Wno-unused-value)
    message(STATUS "Clang build: -fno-strict-aliasing -fwrapv (all targets, set before the toolkit)")
endif()

# ── Profiling symbols ───────────────────────────────────────────
# Release builds also get a .pdb, so a profiler (Visual Studio's Performance
# Profiler) and crash traces can name the generated function (sub_XXXXXXXX)
# that time is spent in. Doesn't change the generated code's speed.
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/Zi;/FS>")
    target_link_options(${PROJECT_NAME} PRIVATE
        "$<$<CONFIG:Release>:/DEBUG:FULL;/OPT:REF;/OPT:NOICF>")
endif()
