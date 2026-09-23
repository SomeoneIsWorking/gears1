cmake_minimum_required(VERSION 3.20)
project(gears1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

# Xenia builds its Debug objects with libstdc++'s debug containers, which have a
# different layout. Every library linked into the same program must agree, so
# the composing build sets it for all of them, including Lucent, rather than
# only for targets that inherit it from x360port.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions($<$<CONFIG:Debug>:_GLIBCXX_DEBUG>)
endif()

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
set(X360PORT_REQUIRED_REVISION "5bf539eac321fd36dc5937f1d791e96ca7042a84")
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

# One source of truth for the binary CTest set. Registering a test also makes
# its executable a dependency of gears1_ctest_binaries, so a verifier that
# builds that target can never run ctest against a binary it did not build.
add_custom_target(gears1_ctest_binaries)
function(gears_add_binary_test test_name executable)
    add_test(NAME ${test_name} COMMAND ${executable})
    add_dependencies(gears1_ctest_binaries ${executable})
endfunction()

add_executable(test_gears1_dynarec_boundary tests/test_gears1_dynarec_boundary.cpp)
target_link_libraries(test_gears1_dynarec_boundary PRIVATE x360port::x360port)
x360port_enable_warnings(test_gears1_dynarec_boundary)
gears_add_binary_test(gears1_dynarec_boundary test_gears1_dynarec_boundary)
add_test(NAME gears_dependency_checkout_contract
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tests/test_dependency_checkout.py")
add_test(NAME gears_import_inventory_contract
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tests/test_import_inventory.py")

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
    GIT_TAG f1494ccb06804ac0c85a6876e5a72bc54d88837e)
FetchContent_MakeAvailable(lucent)
add_library(gears_input STATIC runtime/input.cpp runtime/titles/gears1/xam_input_provider.cpp
                            runtime/titles/gears1/desktop_controls.cpp)
target_include_directories(gears_input PUBLIC runtime)
target_link_libraries(gears_input PUBLIC lucent::lucent x360port::x360port)
x360port_enable_warnings(gears_input)
add_executable(test_remote_input tests/test_remote_input.cpp)
target_link_libraries(test_remote_input PRIVATE gears_input)
x360port_enable_warnings(test_remote_input)
gears_add_binary_test(gears_remote_input test_remote_input)
add_executable(test_script_handoff tests/test_script_handoff.cpp)
target_link_libraries(test_script_handoff PRIVATE gears_input)
x360port_enable_warnings(test_script_handoff)
gears_add_binary_test(gears_script_handoff test_script_handoff)
set_tests_properties(gears_script_handoff PROPERTIES ENVIRONMENT "GEARS_INPUT_SCRIPT=f1:A,f2:")
add_executable(test_gears1_desktop_controls tests/test_gears1_desktop_controls.cpp)
target_link_libraries(test_gears1_desktop_controls PRIVATE gears_input)
x360port_enable_warnings(test_gears1_desktop_controls)
gears_add_binary_test(gears1_desktop_controls test_gears1_desktop_controls)

add_library(gears1_audio_mix STATIC runtime/titles/gears1/audio_mix.cpp)
target_include_directories(gears1_audio_mix PUBLIC runtime)
target_link_libraries(gears1_audio_mix PUBLIC x360port::x360port)
x360port_enable_warnings(gears1_audio_mix)

add_library(gears1_runtime STATIC runtime/gears1_runtime.cpp)
target_include_directories(gears1_runtime PUBLIC runtime)
target_link_libraries(gears1_runtime PUBLIC gears1_guest_image gears1_xam_services gears_input
                                              gears1_audio_mix x360port::x360port)
x360port_enable_warnings(gears1_runtime)
add_executable(test_gears1_runtime tests/test_gears1_runtime.cpp)
target_link_libraries(test_gears1_runtime PRIVATE gears1_runtime)
x360port_enable_warnings(test_gears1_runtime)
gears_add_binary_test(gears1_runtime_composition test_gears1_runtime)

add_executable(test_gears1_guest_image tests/test_gears1_guest_image.cpp)
target_link_libraries(test_gears1_guest_image PRIVATE gears1_guest_image)
x360port_enable_warnings(test_gears1_guest_image)
gears_add_binary_test(gears1_guest_image test_gears1_guest_image)

# This discriminator consumes an operator-supplied XEX and is intentionally
# not a normal CTest: restricted title bytes never become a hosted-test input.
# It is the maintained replacement for the temporary real-leaf probe.
add_executable(test_gears1_real_leaf tests/test_gears1_real_leaf.cpp)
target_link_libraries(test_gears1_real_leaf PRIVATE gears1_runtime)
x360port_enable_warnings(test_gears1_real_leaf)

# The product: the authenticated Gears 1 image on x360port's full console,
# with the title's native overrides and controller arbitration.
add_library(gears1_product_options STATIC runtime/product/product_options.cpp)
target_include_directories(gears1_product_options PUBLIC runtime/product)
x360port_enable_warnings(gears1_product_options)
add_executable(test_gears1_product_options tests/test_gears1_product_options.cpp)
target_link_libraries(test_gears1_product_options PRIVATE gears1_product_options)
x360port_enable_warnings(test_gears1_product_options)
gears_add_binary_test(gears1_product_options test_gears1_product_options)
add_library(gears1_pad_form STATIC runtime/product/pad_form.cpp)
target_include_directories(gears1_pad_form PUBLIC runtime/product)
target_link_libraries(gears1_pad_form PUBLIC gears_input lucent::lucent)
x360port_enable_warnings(gears1_pad_form)
add_executable(test_pad_form tests/test_pad_form.cpp)
target_link_libraries(test_pad_form PRIVATE gears1_pad_form)
x360port_enable_warnings(test_pad_form)
gears_add_binary_test(gears1_pad_form test_pad_form)
# x360port provides the full console only where it has a windowed host (GTK on
# Linux today). Elsewhere the product target is absent by name, and the
# launcher refuses with the same reason; the execution contracts still build.
if(TARGET x360port::system)
    add_executable(gears1
        runtime/product/gears1_main.cpp
        runtime/product/gears1_session.cpp
        runtime/product/offscreen_run.cpp
        runtime/product/control_channel.cpp
        runtime/product/portable_pixmap.cpp)
    target_link_libraries(gears1 PRIVATE gears1_product_options gears1_pad_form gears1_audio_mix
                                         gears_input
                                         x360port::system lucent::lucent)
    x360port_enable_warnings(gears1)
else()
    message(STATUS "gears1: x360port has no full-console host here; the product is not built")
endif()

add_executable(test_gears1_ue3_contract tests/test_gears1_ue3_contract.cpp)
target_link_libraries(test_gears1_ue3_contract PRIVATE x360ue3::x360ue3)
x360port_enable_warnings(test_gears1_ue3_contract)
gears_add_binary_test(gears1_ue3_contract test_gears1_ue3_contract)

if(GEARS_DYNAREC_CONTRACT_ONLY)
    add_test(NAME gears1_dynarec_boundary_quality
        COMMAND ${GEARS_UV_RUN} python
                "${CMAKE_SOURCE_DIR}/tools/check_cpp_quality.py"
                "${CMAKE_BINARY_DIR}")
    add_test(NAME gears_bootstrap_contract
        COMMAND ${GEARS_UV_RUN} python
                "${CMAKE_SOURCE_DIR}/tests/test_bootstrap.py")
    add_test(NAME gears_perf_guest_report
        COMMAND ${GEARS_UV_RUN} python
                "${CMAKE_SOURCE_DIR}/tests/test_perf_guest_report.py")
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
