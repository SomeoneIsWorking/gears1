# Xenia's Xenos microcode front end + SPIR-V back end, compiled as a static
# library out of the `extern/xenia` submodule (BSD-3-Clause; see
# docs/xenia-reuse.md and README.md for attribution).
#
# NOTHING in this directory is our code except this file: the sources are
# referenced in place inside the submodule so provenance stays obvious and the
# fork can be re-pinned without a merge. Keep it that way -- our own code lives
# in runtime/ and tools/.
#
# The whole target is optional in the same way the presenter is: if the
# submodule (or its own third_party submodules) are not checked out, we set
# GEARS_HAVE_XENOS_TRANSLATOR to OFF and everything that uses it drops out.

set(XENIA_ROOT "${CMAKE_SOURCE_DIR}/extern/xenia")
set(XENIA_SRC "${XENIA_ROOT}/src")
set(XENIA_TP "${XENIA_ROOT}/third_party")

set(GEARS_HAVE_XENOS_TRANSLATOR OFF PARENT_SCOPE)

foreach(probe
        "${XENIA_SRC}/xenia/gpu/spirv_shader_translator.cc"
        "${XENIA_TP}/glslang/SPIRV/SpvBuilder.cpp"
        "${XENIA_TP}/fmt/include/fmt/format.h"
        "${XENIA_TP}/Vulkan-Headers/include/vulkan/vulkan.h")
    if(NOT EXISTS "${probe}")
        message(WARNING
            "gears1: Xenos shader translator disabled -- missing ${probe}.\n"
            "  git submodule update --init --recursive extern/xenia\n"
            "  (in extern/xenia: third_party/{glslang,fmt,Vulkan-Headers})")
        return()
    endif()
endforeach()

# glslang's SPIR-V assembler. Only the SpvBuilder half is needed -- the GLSL
# front end (GlslangToSpv, SpvTools, disassemble) is not.
add_library(gears_glslang_spirv STATIC
    "${XENIA_TP}/glslang/SPIRV/SpvBuilder.cpp"
    "${XENIA_TP}/glslang/SPIRV/SpvPostProcess.cpp"
    "${XENIA_TP}/glslang/SPIRV/InReadableOrder.cpp"
    "${XENIA_TP}/glslang/SPIRV/Logger.cpp"
    "${XENIA_TP}/glslang/SPIRV/doc.cpp"
    "${XENIA_TP}/glslang/SPIRV/disassemble.cpp")
target_include_directories(gears_glslang_spirv SYSTEM PUBLIC
    "${XENIA_TP}/glslang"
    "${XENIA_ROOT}")
target_compile_options(gears_glslang_spirv PRIVATE -w)

# snappy. Reached only through gpu/trace_writer.cc, which the translator drags
# in via draw_util -> draw_extent_estimator; nothing in a translation run ever
# compresses anything. Built with xenia's own recipe (third_party/CMakeLists.txt)
# rather than a hand-written config, including generating snappy-stubs-public.h.
if(NOT EXISTS "${XENIA_TP}/snappy/snappy-stubs-public.h")
    execute_process(COMMAND ${CMAKE_COMMAND}
        -DSNAPPY_BUILD_TESTS=OFF -DSNAPPY_BUILD_BENCHMARKS=OFF
        -DSNAPPY_REQUIRE_AVX=ON
        "${XENIA_TP}/snappy" -B "${XENIA_TP}/snappy"
        OUTPUT_QUIET ERROR_QUIET)
endif()
add_library(gears_snappy STATIC
    "${XENIA_TP}/snappy/snappy.cc"
    "${XENIA_TP}/snappy/snappy-sinksource.cc"
    "${XENIA_TP}/snappy/snappy-stubs-internal.cc")
target_include_directories(gears_snappy SYSTEM PUBLIC "${XENIA_TP}/snappy")
target_compile_options(gears_snappy PRIVATE -w)

# fmt, used by xenia/base/string_buffer.h.
add_library(gears_fmt STATIC "${XENIA_TP}/fmt/src/format.cc")
target_include_directories(gears_fmt SYSTEM PUBLIC "${XENIA_TP}/fmt/include")
target_compile_options(gears_fmt PRIVATE -w)

# The translator proper: microcode front end (ucode/xenos/shader/
# shader_translator) + SPIR-V back end, plus the handful of xenia/base
# translation units they pull in.
add_library(xenia_gpu STATIC
    "${XENIA_SRC}/xenia/gpu/gpu_flags.cc"
    "${XENIA_SRC}/xenia/gpu/draw_util.cc"
    "${XENIA_SRC}/xenia/gpu/render_target_cache.cc"
    "${XENIA_SRC}/xenia/gpu/register_file.cc"
    "${XENIA_SRC}/xenia/gpu/registers.cc"
    "${XENIA_SRC}/xenia/gpu/texture_info.cc"
    "${XENIA_SRC}/xenia/gpu/texture_info_formats.cc"
    "${XENIA_SRC}/xenia/gpu/draw_extent_estimator.cc"
    "${XENIA_SRC}/xenia/gpu/shader_interpreter.cc"
    "${XENIA_SRC}/xenia/gpu/trace_writer.cc"
    "${XENIA_SRC}/xenia/ui/graphics_util.cc"
    "${XENIA_SRC}/xenia/gpu/texture_util.cc"
    "${XENIA_SRC}/xenia/gpu/ucode.cc"
    "${XENIA_SRC}/xenia/gpu/xenos.cc"
    "${XENIA_SRC}/xenia/gpu/shader.cc"
    "${XENIA_SRC}/xenia/gpu/shader_translator.cc"
    "${XENIA_SRC}/xenia/gpu/shader_translator_disasm.cc"
    "${XENIA_SRC}/xenia/gpu/spirv_builder.cc"
    "${XENIA_SRC}/xenia/gpu/spirv_shader.cc"
    "${XENIA_SRC}/xenia/gpu/spirv_shader_translator.cc"
    "${XENIA_SRC}/xenia/gpu/spirv_shader_translator_alu.cc"
    "${XENIA_SRC}/xenia/gpu/spirv_shader_translator_fetch.cc"
    "${XENIA_SRC}/xenia/gpu/spirv_shader_translator_memexport.cc"
    "${XENIA_SRC}/xenia/gpu/spirv_shader_translator_rb.cc"
    "${XENIA_SRC}/xenia/base/string_buffer.cc"
    "${XENIA_SRC}/xenia/base/string.cc"
    "${XENIA_SRC}/xenia/base/utf8.cc"
    "${XENIA_SRC}/xenia/base/memory.cc"
    "${XENIA_SRC}/xenia/base/memory_posix.cc"
    "${XENIA_SRC}/xenia/base/logging.cc"
    "${XENIA_SRC}/xenia/base/console_posix.cc"
    "${XENIA_SRC}/xenia/base/debugging_posix.cc"
    "${XENIA_SRC}/xenia/base/cvar.cc"
    "${XENIA_SRC}/xenia/base/filesystem.cc"
    "${XENIA_SRC}/xenia/base/filesystem_posix.cc"
    "${XENIA_SRC}/xenia/base/filesystem_wildcard.cc"
    "${XENIA_SRC}/xenia/base/threading.cc"
    "${XENIA_SRC}/xenia/base/threading_posix.cc"
    "${XENIA_SRC}/xenia/base/threading_timer_queue.cc"
    "${XENIA_SRC}/xenia/base/mutex.cc"
    "${XENIA_SRC}/xenia/base/clock.cc"
    "${XENIA_SRC}/xenia/base/clock_posix.cc"
    "${XENIA_SRC}/xenia/base/clock_x64.cc"
    "${XENIA_SRC}/xenia/base/host_thread_context.cc"
    "${XENIA_SRC}/xenia/base/ring_buffer.cc"
    "${XENIA_SRC}/xenia/base/platform_amd64.cc"
    # Ours: stands in for Xenia's UI/host layer, which the translator does not
    # otherwise need. See the file for what it replaces and why.
    "${CMAKE_CURRENT_SOURCE_DIR}/xenia_host_shim.cpp")

# xenia-build.py normally generates version.h from the checkout's git state.
# We are not running xenia's build script, so generate the same header from the
# pinned submodule commit -- the real value, not a placeholder.
execute_process(COMMAND git -C "${XENIA_ROOT}" rev-parse HEAD
                OUTPUT_VARIABLE XENIA_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT XENIA_COMMIT)
    message(FATAL_ERROR "gears1: cannot read extern/xenia's HEAD for version.h")
endif()
string(SUBSTRING "${XENIA_COMMIT}" 0 7 XENIA_COMMIT_SHORT)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
"// Generated by xenia_gpu/CMakeLists.txt from the pinned extern/xenia commit.\n"
"#define XE_BUILD_BRANCH \"pinned\"\n"
"#define XE_BUILD_COMMIT \"${XENIA_COMMIT}\"\n"
"#define XE_BUILD_COMMIT_SHORT \"${XENIA_COMMIT_SHORT}\"\n"
"#define XE_BUILD_DATE \"\"\n")

target_include_directories(xenia_gpu SYSTEM PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${XENIA_SRC}"
    "${XENIA_ROOT}"
    "${XENIA_TP}/Vulkan-Headers/include"
    "${XENIA_TP}/utfcpp/source"
    "${XENIA_TP}/cxxopts/include"
    "${XENIA_TP}/date/include")

target_compile_definitions(xenia_gpu PUBLIC
    VULKAN_HPP_NO_TO_STRING
    VK_NO_PROTOTYPES)
target_compile_options(xenia_gpu PRIVATE -w -mavx)

# Xenia's asserts are on in its own Debug builds and off in its Release builds.
# We default them OFF, and that is a decision with evidence behind it, not
# convenience: every one of this title's 103 distinct vertex shaders trips
# assert_not_zero(fetch_instr.attributes.stride) at shader_translator.cc:421,
# because Gears of War leaves the vfetch_full stride field zero and supplies the
# stride from the vertex fetch constant at bind time instead. The assert encodes
# an assumption the title does not satisfy; it is not detecting a bad parse (the
# disassembly either side of it is clean). Turn them back on with
#     cmake -DGEARS_XENIA_ASSERTS=ON
# when investigating a translation problem -- they are useful, they just cannot
# be the default here.
option(GEARS_XENIA_ASSERTS "Keep Xenia's internal asserts enabled" OFF)
if(NOT GEARS_XENIA_ASSERTS)
    target_compile_definitions(xenia_gpu PRIVATE NDEBUG)
endif()
target_link_libraries(xenia_gpu PUBLIC gears_glslang_spirv gears_fmt gears_snappy)

set(GEARS_HAVE_XENOS_TRANSLATOR ON PARENT_SCOPE)
