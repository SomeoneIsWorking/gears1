# The headless Xenia oracle harness (see main.cpp for what it is and why).
#
# This is a SEPARATE cmake project from the gears1 build on purpose. It needs
# the whole emulator -- cpu, kernel, apu, hid, vfs, gpu, ui -- and building all
# of that as part of every gears1 configure would cost minutes for something
# only the oracle uses. It is also NOT a copy of Xenia's source lists: it adds
# extern/xenia as a subdirectory and links the targets Xenia's own CMake
# defines, so re-pinning the submodule cannot leave a stale list behind.
#
# Configure and build (from the repo root):
#
#   cmake -S tools/xenia_oracle -B scratch/oracle/oracle-build -G Ninja \
#         -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS=-I<abs>/generated \
#         -DCMAKE_EXE_LINKER_FLAGS=-L<abs>/scratch/oracle/localdev
#   ninja -C scratch/oracle/oracle-build xenia_oracle
#
# RelWithDebInfo rather than Release because Xenia's Release config forces
# -fuse-ld=lld, which is not installed here; see docs/xenia-reuse.md.
cmake_minimum_required(VERSION 3.16)
project(gears_xenia_oracle LANGUAGES C CXX)

# Xenia sets this in its own CMakeLists, which does NOT reach a target declared
# in this (parent) scope -- and its headers use C++20 requires-clauses, so a
# target without it fails to parse xenia/base/chrono.h with a syntax error that
# points at Xenia rather than at the missing standard.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(XENIA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../extern/xenia")
if(NOT EXISTS "${XENIA_ROOT}/src/xenia/emulator.cc")
    message(FATAL_ERROR
        "gears1: extern/xenia is not checked out, so the oracle cannot be "
        "built. git submodule update --init --recursive extern/xenia")
endif()

# Xenia's own build. XENIA_BUILD_MISC brings in the console GPU tools; we do not
# need them, but leaving it ON keeps this build interchangeable with the one
# documented in docs/xenia-reuse.md rather than producing a subtly different
# emulator.
set(XENIA_BUILD_MISC ON CACHE BOOL "" FORCE)
add_subdirectory("${XENIA_ROOT}" xenia)

add_executable(xenia_oracle
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    # The console entry point, the same one Xenia's own console tools use. It
    # is what initialises logging on POSIX -- without it the first log call
    # blocks forever (catalog #7).
    "${XENIA_ROOT}/src/xenia/base/console_app_main_posix.cc")

target_include_directories(xenia_oracle PRIVATE
    "${XENIA_ROOT}"
    "${XENIA_ROOT}/src"
    "${XENIA_ROOT}/third_party/Vulkan-Headers/include"
    "${XENIA_ROOT}/third_party/glslang")

# The same link set as xenia-gpu-vulkan-trace-dump, which is the closest thing
# in the tree to what this is: a console app that drives the emulator core.
# -nop drivers for audio and input, because a headless run has neither device.
target_link_libraries(xenia_oracle PRIVATE
    xenia-apu xenia-apu-nop xenia-base xenia-core xenia-cpu
    xenia-gpu xenia-gpu-vulkan xenia-hid xenia-hid-nop
    xenia-kernel xenia-patcher xenia-ui xenia-ui-vulkan xenia-vfs
    aes_128 capstone fmt glslang-spirv imgui libavcodec libavutil mspack
    snappy xxhash)
# XE_TARGET_X86_64 is set inside Xenia's own scope and is not visible here, so
# the architecture is determined directly. Without the JIT backend the link
# fails on X64Backend's constructor -- an emulator with no way to execute guest
# code, which is not a configuration worth building.
if(XE_TARGET_X86_64 OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    target_link_libraries(xenia_oracle PRIVATE xenia-cpu-backend-x64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    target_link_libraries(xenia_oracle PRIVATE xenia-cpu-backend-a64)
else()
    message(FATAL_ERROR "gears1: no Xenia CPU backend for ${CMAKE_SYSTEM_PROCESSOR}")
endif()

xe_target_defaults(xenia_oracle)

# xe_target_defaults turns warnings into errors, and this translation unit pulls
# in Xenia headers that trip them (xam/user_data.h has an unhandled-enum switch).
# Those are THEIR warnings in THEIR headers -- fixing them belongs upstream, and
# silencing them here does not weaken any check on our own code, which is one
# file of straight-line setup.
target_compile_options(xenia_oracle PRIVATE -Wno-error)
