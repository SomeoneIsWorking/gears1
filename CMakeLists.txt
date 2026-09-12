cmake_minimum_required(VERSION 3.20)
project(gears1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

option(GEARS_REQUIRE_PRODUCT_DEPS
    "Refuse a reduced build that cannot run the current GearsUE3 product" OFF)
option(GEARS_DYNAREC_CONTRACT_ONLY
    "Build only the asset-free Gears/x360port execution discriminator" OFF)

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

set(X360PORT_ROOT "${CMAKE_SOURCE_DIR}/../../shared/x360port" CACHE PATH
    "Exact shared/x360port checkout consumed by GearsUE3")
set(X360PORT_REQUIRED_REVISION "d5389c79f8ebcf1d90ff81029820c5967f9ca34d")
execute_process(
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tools/require_git_checkout.py"
            --label shared/x360port
            --root "${X360PORT_ROOT}"
            --revision "${X360PORT_REQUIRED_REVISION}"
            --required-file CMakeLists.txt
    RESULT_VARIABLE X360PORT_REVISION_RESULT
    OUTPUT_VARIABLE X360PORT_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE X360PORT_REVISION_ERROR
    ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT X360PORT_REVISION_RESULT EQUAL 0)
    message(FATAL_ERROR
        "GearsUE3 dependency refusal: ${X360PORT_REVISION_ERROR}")
endif()
set(X360PORT_XENIA_SOURCE_DIR "${CMAKE_SOURCE_DIR}/extern/xenia" CACHE PATH
    "Exact maintained Xenia checkout consumed through x360port" FORCE)
add_subdirectory("${X360PORT_ROOT}" x360port EXCLUDE_FROM_ALL)
if(NOT TARGET x360port)
    message(FATAL_ERROR
        "shared/x360port exists but does not expose its Xenia-backed runtime target")
endif()
message(STATUS
    "gears1: shared/x360port ${X360PORT_REVISION} Xenia runtime resolved")

set(X360UE3_ROOT "${CMAKE_SOURCE_DIR}/../../shared/x360ue3" CACHE PATH
    "Exact shared/x360ue3 checkout consumed by GearsUE3")
set(X360UE3_REQUIRED_REVISION "7bc5763b9a3acb3ea04463ef05056b222a2579be")
execute_process(
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tools/require_git_checkout.py"
            --label shared/x360ue3
            --root "${X360UE3_ROOT}"
            --revision "${X360UE3_REQUIRED_REVISION}"
            --required-file CMakeLists.txt
    RESULT_VARIABLE X360UE3_REVISION_RESULT
    OUTPUT_VARIABLE X360UE3_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE X360UE3_REVISION_ERROR
    ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT X360UE3_REVISION_RESULT EQUAL 0)
    message(FATAL_ERROR
        "GearsUE3 dependency refusal: ${X360UE3_REVISION_ERROR}")
endif()
add_subdirectory("${X360UE3_ROOT}" x360ue3 EXCLUDE_FROM_ALL)
message(STATUS
    "gears1: shared/x360ue3 ${X360UE3_REVISION} contract resolved")

add_executable(test_gears1_dynarec_boundary tests/test_gears1_dynarec_boundary.cpp)
target_link_libraries(test_gears1_dynarec_boundary PRIVATE x360port::x360port)
x360port_enable_warnings(test_gears1_dynarec_boundary)
add_test(NAME gears1_dynarec_boundary COMMAND test_gears1_dynarec_boundary)
add_test(NAME gears_dependency_checkout_contract
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tests/test_dependency_checkout.py")

add_library(gears1_guest_image STATIC runtime/gears1_guest_image.cpp)
target_include_directories(gears1_guest_image PUBLIC runtime)
target_link_libraries(gears1_guest_image PUBLIC x360port::x360port)
x360port_enable_warnings(gears1_guest_image)
add_library(gears1_xam_services STATIC runtime/titles/gears1/xam_video_services.cpp)
target_include_directories(gears1_xam_services PUBLIC runtime)
target_link_libraries(gears1_xam_services PUBLIC x360port::x360port)
x360port_enable_warnings(gears1_xam_services)

include(FetchContent)
FetchContent_Declare(lucent
    GIT_REPOSITORY https://github.com/SomeoneIsWorking/lucent.git
    GIT_TAG f1101923eee7451c5d5aa25a25d68bfb4ae874b1)
FetchContent_MakeAvailable(lucent)
add_library(gears_input STATIC runtime/input.cpp runtime/titles/gears1/xam_input_provider.cpp)
target_include_directories(gears_input PUBLIC runtime)
target_link_libraries(gears_input PUBLIC lucent::lucent x360port::x360port)
x360port_enable_warnings(gears_input)
add_executable(test_remote_input tests/test_remote_input.cpp)
target_link_libraries(test_remote_input PRIVATE gears_input)
x360port_enable_warnings(test_remote_input)
add_test(NAME gears_remote_input COMMAND test_remote_input)

add_executable(test_gears1_guest_image tests/test_gears1_guest_image.cpp)
target_link_libraries(test_gears1_guest_image PRIVATE gears1_guest_image)
x360port_enable_warnings(test_gears1_guest_image)
add_test(NAME gears1_guest_image COMMAND test_gears1_guest_image)

# This discriminator consumes an operator-supplied XEX and is intentionally
# not a normal CTest: restricted title bytes never become a hosted-test input.
# It is the maintained replacement for the temporary real-leaf probe.
add_executable(test_gears1_real_leaf tests/test_gears1_real_leaf.cpp)
target_link_libraries(test_gears1_real_leaf PRIVATE gears1_guest_image gears1_xam_services
                                                    gears_input x360port::x360port)
x360port_enable_warnings(test_gears1_real_leaf)

add_executable(test_gears1_ue3_contract tests/test_gears1_ue3_contract.cpp)
target_link_libraries(test_gears1_ue3_contract PRIVATE x360ue3::x360ue3)
x360port_enable_warnings(test_gears1_ue3_contract)
add_test(NAME gears1_ue3_contract COMMAND test_gears1_ue3_contract)

if(GEARS_DYNAREC_CONTRACT_ONLY)
    add_test(NAME gears1_dynarec_boundary_quality
        COMMAND ${GEARS_UV_RUN} python
                "${CMAKE_SOURCE_DIR}/tools/check_cpp_quality.py"
                "${CMAKE_BINARY_DIR}")
    add_test(NAME gears_bootstrap_contract
        COMMAND ${GEARS_UV_RUN} python
                "${CMAKE_SOURCE_DIR}/tests/test_bootstrap.py")
    return()
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
