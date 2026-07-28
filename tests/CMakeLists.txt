# Correctness tests for the instruction implementations added to our
# XenonRecomp fork. These need only the generated ppc_context.h and simde.
add_executable(test_vmx_instructions test_vmx_instructions.cpp)
target_include_directories(test_vmx_instructions PRIVATE
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde")
add_test(NAME vmx_instructions COMMAND test_vmx_instructions)

# Tests for the runtime's own logic: kernel object semantics and path
# translation. These link the real runtime sources, not copies.
add_executable(test_runtime_logic
    test_runtime_logic.cpp
    ../runtime/kernel_objects.cpp
    ../runtime/guest_filesystem.cpp
    ../runtime/guest_heap.cpp
    ../runtime/guest_memory.cpp)
target_include_directories(test_runtime_logic PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde"
    ../extern/XenonRecomp/XenonUtils)
target_link_libraries(test_runtime_logic PRIVATE lucent::lucent)
add_test(NAME runtime_logic COMMAND test_runtime_logic)

# Replays a dumped XMA context (GEARS_XMA_DUMP's .ctx + .packets pair) through
# the REAL context protocol -- xma_context.cpp, the same code the runtime
# runs -- acting as the title: kick, drain the output ring, advance the read
# offset, repeat. Its PCM output is what the golden ffmpeg reference is
# compared against (tools/xma_compare.py). Not a test in the ctest sense (it
# needs a dump), but built with the tests so it cannot bit-rot.
if(GEARS_HAVE_XMA_DECODER)
    add_executable(xma_replay
        ../tools/xma_replay.cpp
        ../runtime/xma_context.cpp
        ../runtime/xma_decode.cpp
        ../runtime/guest_memory.cpp)
    target_include_directories(xma_replay PRIVATE
        ../runtime
        "${GEARS_PPC_DIR}"
        "${THIRDPARTY_ROOT}/simde"
        ../extern/XenonRecomp/XenonUtils)
    target_compile_definitions(xma_replay PRIVATE GEARS_HAVE_XMA_DECODER=1)
    target_link_libraries(xma_replay PRIVATE ffmpeg_xma lucent::lucent)
endif()

# Replays a recorded heap trace through the real allocator. Not a test (it
# needs a captured trace), but built with the tests so it cannot bit-rot.
add_executable(heap_replay
    ../tools/heap_replay.cpp
    ../runtime/guest_heap.cpp
    ../runtime/guest_memory.cpp)
target_include_directories(heap_replay PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde"
    ../extern/XenonRecomp/XenonUtils)
target_link_libraries(heap_replay PRIVATE lucent::lucent)
