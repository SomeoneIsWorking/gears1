cmake_minimum_required(VERSION 3.20)
project(gears1 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

add_subdirectory(runtime)

enable_testing()
add_subdirectory(tests)
