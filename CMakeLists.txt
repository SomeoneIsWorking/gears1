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

set(X360PORT_ROOT "${CMAKE_SOURCE_DIR}/../../shared/x360port")
set(X360PORT_REQUIRED_REVISION "1bbe9c195c39607dbdd0cb407f59d6d9c79c62f2")
if(NOT EXISTS "${X360PORT_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "GearsUE3 requires the shared/x360port checkout at ${X360PORT_ROOT}; "
        "no in-tree or downloaded fallback is allowed")
endif()
find_package(Git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${X360PORT_ROOT}" rev-parse HEAD
    RESULT_VARIABLE X360PORT_REVISION_RESULT
    OUTPUT_VARIABLE X360PORT_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT X360PORT_REVISION_RESULT EQUAL 0 OR
   NOT X360PORT_REVISION STREQUAL X360PORT_REQUIRED_REVISION)
    message(FATAL_ERROR
        "GearsUE3 requires shared/x360port revision "
        "${X360PORT_REQUIRED_REVISION}, observed '${X360PORT_REVISION}' at "
        "${X360PORT_ROOT}")
endif()
add_subdirectory("${X360PORT_ROOT}" x360port EXCLUDE_FROM_ALL)
if(NOT TARGET x360port_validation)
    message(FATAL_ERROR
        "shared/x360port exists but does not expose its validated "
        "authenticated-module contract target x360port_validation")
endif()
message(STATUS
    "gears1: shared/x360port ${X360PORT_REVISION} validation contract resolved")
if(GEARS_REQUIRE_PRODUCT_DEPS AND NOT TARGET x360port)
    message(FATAL_ERROR
        "GearsUE3 has no gameplay executor: the required shared/x360port "
        "Xenia dynarec boundary is not implemented. The retired generated-code "
        "product cannot be generated, built, or selected.")
endif()

# Renderer-only xxHash use is independent of guest CPU execution. Consume the
# pinned Xenia copy directly until x360port owns the complete Xenia build.
set(GEARS_XXHASH_ROOT "${CMAKE_SOURCE_DIR}/extern/xenia/third_party/xxhash")
if(EXISTS "${GEARS_XXHASH_ROOT}/xxhash.c")
    add_library(gears_xxhash STATIC "${GEARS_XXHASH_ROOT}/xxhash.c")
    target_include_directories(gears_xxhash PUBLIC "${GEARS_XXHASH_ROOT}")
    add_library(xxHash::xxhash ALIAS gears_xxhash)
endif()

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
