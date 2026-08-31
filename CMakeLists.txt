cmake_minimum_required(VERSION 3.20)
project(gears1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

option(GEARS_REQUIRE_PRODUCT_DEPS
    "Refuse a reduced build that cannot run the current GearsUE3 product" OFF)

# Python tools run in one locked uv project. Never select an ambient Python:
# that made CTest and interactive diagnostics exercise different dependencies.
find_program(GEARS_UV_EXECUTABLE NAMES uv REQUIRED)
set(Python3_EXECUTABLE "" CACHE FILEPATH
    "Exact locked Python interpreter supplied by the GearsUE3 initializer")
if(NOT Python3_EXECUTABLE)
    execute_process(
        COMMAND "${GEARS_UV_EXECUTABLE}" run --locked
                --project "${CMAKE_SOURCE_DIR}" python -c
                "import sys; print(sys.executable)"
        RESULT_VARIABLE GEARS_PYTHON_DISCOVERY_RESULT
        OUTPUT_VARIABLE Python3_EXECUTABLE
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT GEARS_PYTHON_DISCOVERY_RESULT EQUAL 0 OR NOT Python3_EXECUTABLE)
        message(FATAL_ERROR
            "Cannot resolve the locked Python interpreter through uv")
    endif()
    set(Python3_EXECUTABLE "${Python3_EXECUTABLE}" CACHE FILEPATH
        "Exact locked Python interpreter supplied by the GearsUE3 initializer" FORCE)
endif()
set(GEARS_UV_RUN
    "${GEARS_UV_EXECUTABLE}" run --locked --project "${CMAKE_SOURCE_DIR}"
    --python "${Python3_EXECUTABLE}")

# The project accepts the compiler supported by the host and its dependencies.
# Maintainer verification selects Clang explicitly, but that policy must not
# turn into a project-local compiler ban for users.

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

if(GEARS_REQUIRE_PRODUCT_DEPS AND NOT GEARS_HAVE_XENOS_TRANSLATOR)
    message(FATAL_ERROR
        "GearsUE3 product build requires the extern/xenia Xenos translator; "
        "initialize the pinned submodules")
endif()

# The XMA audio decoder (extern/ffmpeg-xmaframes). Optional in the same way the
# Xenos translator is: without the submodule, the build simply has no decoder.
set(GEARS_HAVE_XMA_DECODER OFF)
include(cmake/ffmpeg_xma.cmake)

if(GEARS_REQUIRE_PRODUCT_DEPS AND NOT GEARS_HAVE_XMA_DECODER)
    message(FATAL_ERROR
        "GearsUE3 product build requires the pinned XMA decoder; initialize "
        "extern/ffmpeg-xmaframes and its dependencies")
endif()

add_subdirectory(runtime)

if(GEARS_HAVE_XENOS_TRANSLATOR)
    add_subdirectory(tools/xenos_translate)
    add_subdirectory(tools/system_constants)
endif()

add_subdirectory(tests)
