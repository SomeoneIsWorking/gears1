cmake_minimum_required(VERSION 3.20)
project(gears1 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "XenonRecomp's output relies on Clang-specific intrinsics and function "
        "aliasing. Configure with -DCMAKE_CXX_COMPILER=clang++.")
endif()

# Where XenonRecomp wrote the translated game code. Gitignored: it is derived
# from the user's own copy of the game and never enters the repository.
set(GEARS_PPC_DIR "${CMAKE_SOURCE_DIR}/scratch/ppc"
    CACHE PATH "Directory containing XenonRecomp's generated ppc_recomp.*.cpp")

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

# The licensed UE3 checkout stays external. When configured, build its native
# host port through the narrow adapter in native/ue3; no UE3 source is copied
# into this repository.
set(RETIRED_PRIVATE_SOURCE_INPUT "$ENV{RETIRED_PRIVATE_SOURCE_INPUT}" CACHE PATH
    "External UE3 source checkout used by the native engine target")
if(RETIRED_PRIVATE_SOURCE_INPUT)
    if(NOT EXISTS "${RETIRED_PRIVATE_SOURCE_INPUT}/<retired-private-source-tree>/Core/Inc/Core.h")
        message(FATAL_ERROR
            "RETIRED_PRIVATE_SOURCE_INPUT='${RETIRED_PRIVATE_SOURCE_INPUT}' does not contain "
            "<retired-private-source-tree>/Core/Inc/Core.h")
    endif()
    add_subdirectory(native/ue3)
else()
    message(STATUS
        "Native UE3 target unavailable: set RETIRED_PRIVATE_SOURCE_INPUT to the external source checkout")
endif()

add_subdirectory(runtime)

if(GEARS_HAVE_XENOS_TRANSLATOR)
    add_subdirectory(tools/xenos_translate)
    add_subdirectory(tools/system_constants)
endif()

add_subdirectory(tests)
