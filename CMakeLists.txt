cmake_minimum_required(VERSION 3.20)
project(gears1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

# Python tools run in one locked uv project. Never select an ambient Python:
# that made CTest and interactive diagnostics exercise different dependencies.
find_program(GEARS_UV_EXECUTABLE NAMES uv REQUIRED)
set(GEARS_UV_RUN
    "${GEARS_UV_EXECUTABLE}" run --locked --project "${CMAKE_SOURCE_DIR}")

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "XenonRecomp's output relies on Clang-specific intrinsics and function "
        "aliasing. Configure with -DCMAKE_CXX_COMPILER=clang++.")
endif()

# Where XenonRecomp wrote the translated game code. Gitignored: it is derived
# from the user's own copy of the game and never enters the repository.
set(GEARS_PPC_DIR "${CMAKE_SOURCE_DIR}/scratch/ppc"
    CACHE PATH "Directory containing XenonRecomp's generated ppc_recomp.*.cpp")
set(GEARS_GAME_DIR "$ENV{GEARS_GAME_DIR}"
    CACHE PATH "Extracted Gears game directory used by required content gates")
if(NOT GEARS_GAME_DIR)
    set(GEARS_GAME_DIR "${CMAKE_SOURCE_DIR}/scratch/game"
        CACHE PATH "Extracted Gears game directory used by required content gates" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(lucent
    GIT_REPOSITORY https://github.com/SomeoneIsWorking/lucent.git
    GIT_TAG main)
FetchContent_MakeAvailable(lucent)

# XenonUtils gives us the XEX parser, so the runtime loads the game's own file
# rather than depending on a pre-extracted image sitting in scratch/.
set(THIRDPARTY_ROOT ${CMAKE_SOURCE_DIR}/extern/XenonRecomp/thirdparty)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${THIRDPARTY_ROOT} thirdparty EXCLUDE_FROM_ALL)
add_subdirectory(extern/XenonRecomp/XenonUtils XenonUtils EXCLUDE_FROM_ALL)

# Xenia's Xenos microcode front end + SPIR-V back end (BSD-3, extern/xenia).
# Optional in exactly the same way the Vulkan/SDL3 presenter is: if the
# submodule is not checked out this sets GEARS_HAVE_XENOS_TRANSLATOR to OFF and
# everything downstream of it drops out of the build.
add_subdirectory(xenia_gpu)

# The XMA audio decoder (extern/ffmpeg-xmaframes). Optional in the same way the
# Xenos translator is: without the submodule, the build simply has no decoder.
set(GEARS_HAVE_XMA_DECODER OFF)
include(cmake/ffmpeg_xma.cmake)

add_subdirectory(runtime)

if(GEARS_HAVE_XENOS_TRANSLATOR)
    add_subdirectory(tools/xenos_translate)
    add_subdirectory(tools/system_constants)
endif()

add_subdirectory(tests)
