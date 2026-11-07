cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Conker: Live & Reloaded - Static Recompilation
#
# Based on xboxrecomp/templates/new-game. The repo root is the game project
# and the toolkit is the xboxrecomp/ submodule (docs/architecture.md, decision 4).
#
# Configure with the game folder so the .exe finds default.xbe without
# copying any game files (they stay read-only, outside the repo):
#   cmake -S . -B build -DCONKER_GAME_DIR=D:/path/to/extracted
# The .exe is conker.exe, the same file a release ships (SPEC 9).
# ============================================================================

# The version shown in the .exe's properties and used by the launcher's
# update check: the VERSION file at the repo root (SPEC 9.1).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CONKER_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)
project(conker_lr_recomp VERSION ${CONKER_VERSION} LANGUAGES C CXX)

# The C runtime linked in (/MT), so a player needs no Visual C++
# redistributable (SPEC 9.2). Before any target, so the toolkit, ImGui and
# SDL get it too.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# C++ only for the F2 menu (src/overlay.cpp) and Dear ImGui.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
# The toolkit is the xboxrecomp/ submodule inside this repo.
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# ABI check: every lifted call logs ([ABI] lines on stderr) when it returns
# with ebx/esi/edi changed or esp too low, i.e. a missing epilogue or a wrong
# ret N. Off by default; build it into its own folder (build-abi/) for play
# sessions that hunt a crash. Must be defined before the toolkit is added, so
# the logger in xbox_kernel is compiled too (toolkit doc pipeline/05-runtime.md).
# The CRT helpers listed in pipeline/06-debugging.md are always reported.
option(RECOMP_ABI_CHECK "Log callee ABI violations at every lifted call" OFF)
if(RECOMP_ABI_CHECK)
    add_compile_definitions(RECOMP_ABI_CHECK=1)
endif()

# SDL3 for PlayStation controllers (DualShock 4, DualSense): the
# third_party/SDL submodule, pinned to a release tag, built static with only
# what its gamepad API needs. The toolkit's input layer reads them through
# it (xboxrecomp/src/input/sdl_pads.c); Xbox pads stay on XInput.
set(SDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL")
if(NOT EXISTS "${SDL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "third_party/SDL is empty: run "
                        "git submodule update --init")
endif()
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
foreach(_sub AUDIO VIDEO GPU RENDER CAMERA HAPTIC POWER DIALOG TRAY)
    set(SDL_${_sub} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(${SDL_DIR} ${CMAKE_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)
set(XBOXRECOMP_SDL3_TARGET SDL3::SDL3-static CACHE STRING "" FORCE)

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
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
    src/sample_profiler.c
    src/settings.c
    src/controls.c
    src/photo_mode.c
    src/decensor.c
    src/save_names.c
    src/shadows.c
    src/lightmaps.c
    src/overlay.cpp
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# Dear ImGui for the F2 settings menu: the third_party/imgui submodule,
# pinned to a release tag. Only the core and the D3D11 renderer; input is
# fed by src/overlay.cpp itself (the swap chain sits on a child view, so
# the stock Win32 back end would get mouse positions wrong).
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "third_party/imgui is empty: run "
                        "git submodule update --init")
endif()
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR})
# No imgui_demo.cpp: ShowDemoWindow is never called.
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME conker)

# Icon and version info. The icon is the user's own art, kept outside the
# repo like the title logo: scripts/make_art.py copies it into art/ (SPEC
# 9.1). Without it the .exe builds with the default icon. Icon resource 1
# is also the window's (the toolkit's fb_present.c).
set(_conker_ico "${CMAKE_CURRENT_SOURCE_DIR}/art/conker.ico")
if(EXISTS "${_conker_ico}")
    set(CONKER_ICON_LINE "1 ICON \"${_conker_ico}\"")
else()
    message(WARNING "art/conker.ico missing (run scripts/make_art.py): "
                    "conker.exe gets the default icon")
    set(CONKER_ICON_LINE "")
endif()
configure_file(src/conker.rc.in "${CMAKE_CURRENT_BINARY_DIR}/conker.rc" @ONLY)
target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/conker.rc")

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp imgui)

# Include paths for generated recomp headers. Our pipeline puts all of them
# (recomp_types.h, recomp_funcs.h) in src/recomp/gen/ next to the C files.
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen
    ${XBOXRECOMP_DIR}/include
)

# The xemu audio emulation (LGPL) is its own xemu_apu.dll, loaded from the
# .exe's folder (SPEC 9.5; toolkit src/apu/apu_host.h). Its .pdb goes along
# for the crash handler and the sampling profiler. Copied by a target of its
# own on every build: a post-build step on the .exe ran only when the .exe
# relinked, so a change inside the DLL alone left the old copy there (and a
# release would have packaged it; to-do 6, 2026-11-06).
add_custom_target(xemu_apu_copy ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:xemu_apu> $<TARGET_PDB_FILE:xemu_apu>
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
    VERBATIM)
add_dependencies(xemu_apu_copy xemu_apu ${PROJECT_NAME})

# The game folder for a dev build: the .exe looks in "game" next to itself,
# as a release does once the launcher has imported the disc (SPEC 9.3).
# Given CONKER_GAME_DIR, each build makes that "game" a junction to it, so
# nothing is copied and no path is compiled in (the .exe is the one that
# ships). The junction lives in the build folder, never in source; the
# game never writes into the game folder (SPEC 3.1).
set(CONKER_GAME_DIR "" CACHE PATH
    "Folder with Conker's extracted game files (default.xbe)")
if(CONKER_GAME_DIR)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DEXE_DIR=$<TARGET_FILE_DIR:${PROJECT_NAME}>
                -DGAME_DIR=${CONKER_GAME_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/link_game_dir.cmake
        VERBATIM)
endif()

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    xinput
    winmm
    # Photo mode's PNG photos (WIC, overlay.cpp).
    windowscodecs
    ole32
    # The photo panel's folder dialog and Explorer (overlay.cpp).
    shell32
    # imgui_impl_dx11 compiles its two small shaders at start-up.
    d3dcompiler
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
    # SHA-1 of default.xbe: is it a version we run (main.c).
    bcrypt
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)

    # A .pdb next to the .exe, so the crash handler and the sampling profiler
    # (RECOMP_SAMPLE_PROFILE) can name functions. /DEBUG alone gives every
    # generated function (sub_XXXXXXXX) its name; /Zi on our code and the
    # toolkit adds the static runtime functions (xf_rows, ...). Generated C
    # stays without /Zi: it would slow the 13-minute build for nothing.
    # /DEBUG turns off /OPT:REF,ICF by default; they are put back so the code
    # is the same as without symbols.
    target_link_options(${PROJECT_NAME} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    set_source_files_properties(${GAME_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/Zi;/FS")
    foreach(_lib xbox_kernel xbox_d3d8 xbox_dsound xbox_input xbox_usb
                 xbox_apu xbox_apu_emu xbox_nv2a xbox_video)
        target_compile_options(${_lib} PRIVATE /Zi /FS)
    endforeach()
    target_link_options(xemu_apu PRIVATE /DEBUG /OPT:REF /OPT:ICF)
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
