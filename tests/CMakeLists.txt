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
    ../runtime/guest_memory.cpp
    ../runtime/guest_thread.cpp)
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

# The user profile store and the XamUserReadProfileSettings buffer protocol.
# Written before the implementation: the layouts are the console's, so the
# tests encode the contract rather than describing whatever the code does.
add_executable(test_user_profile
    test_user_profile.cpp
    ../runtime/user_profile.cpp)
target_include_directories(test_user_profile PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde"
    ../extern/XenonRecomp/XenonUtils)
target_link_libraries(test_user_profile PRIVATE lucent::lucent)
add_test(NAME user_profile COMMAND test_user_profile)

# The Xam application message dispatch: the media player, the gaming service
# and the Live client. Written before the implementation, against Xenia's
# transcription of the console's message numbers and structures.
add_executable(test_xam_apps
    test_xam_apps.cpp
    ../runtime/xam_apps.cpp)
target_include_directories(test_xam_apps PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde"
    ../extern/XenonRecomp/XenonUtils)
target_link_libraries(test_xam_apps PRIVATE lucent::lucent)
add_test(NAME xam_apps COMMAND test_xam_apps)

# Saved-game content: the XCONTENT_DATA descriptor's byte layout, and the rule
# for when content exists at all.
add_executable(test_xam_content
    test_xam_content.cpp
    ../runtime/xam_content.cpp)
target_include_directories(test_xam_content PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde"
    ../extern/XenonRecomp/XenonUtils)
target_link_libraries(test_xam_content PRIVATE lucent::lucent)
add_test(NAME xam_content COMMAND test_xam_content)

# Directory listings in the console's FILE_DIRECTORY_INFORMATION form. The
# chaining rule is the part that matters: a walker follows next_entry_offset
# until it reads zero.
add_executable(test_directory_info
    test_directory_info.cpp
    ../runtime/directory_info.cpp)
target_include_directories(test_directory_info PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde"
    ../extern/XenonRecomp/XenonUtils)
target_link_libraries(test_directory_info PRIVATE lucent::lucent)
add_test(NAME directory_info COMMAND test_directory_info)

# The console configuration table. Bare numbers with no structure to check them
# against, so the test pins each console NAME to its NUMBER -- a shifted table
# answers every lookup successfully with the wrong setting.
add_executable(test_xconfig
    test_xconfig.cpp
    ../runtime/xconfig.cpp
    ../runtime/host_time_zone.cpp
    ../runtime/kernel_config.cpp)
target_include_directories(test_xconfig PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde"
    ../extern/XenonRecomp/XenonUtils)
target_link_libraries(test_xconfig PRIVATE lucent::lucent)
add_test(NAME xconfig COMMAND test_xconfig)

# XamUserGetName at the guest seam. The console truncates into a short buffer
# instead of refusing it, and Gears calls it with a four-byte one -- a refusal
# is a status the title never checks, so nothing but this test can catch it.
add_executable(test_xam_user
    test_xam_user.cpp
    ../runtime/xam_user.cpp
    ../runtime/user_profile.cpp
    ../runtime/xam_content.cpp
    ../runtime/xam_overlapped.cpp
    ../runtime/guest_filesystem.cpp
    ../runtime/guest_heap.cpp
    ../runtime/guest_memory.cpp
    ../runtime/input.cpp
    ../runtime/kernel_objects.cpp)
target_include_directories(test_xam_user PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde"
    ../extern/XenonRecomp/XenonUtils)
target_link_libraries(test_xam_user PRIVATE lucent::lucent)
add_test(NAME xam_user COMMAND test_xam_user)

# THE INSTRUMENTS' OWN SELF-TESTS, wired into ctest so they cannot rot.
#
# Six diagnostics were caught this session reporting a negative their method
# could never have contradicted -- a scanner blind to one of two address shapes,
# a vtable scan blind to the abstract case, a dataflow check whose registers were
# computed and never used, a probe capped at 4 of 78,278 events, a catalog
# searching a directory that did not exist, and a roadmap tool certifying the
# project finished when its file was missing. Each now has a self-test that
# feeds it a case which MUST produce a positive.
#
# A self-test nobody runs is the same failure one level up, so they run here.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME instrument_find_addr_refs
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/find_addr_refs.py" --selftest)
    add_test(NAME instrument_atomic_audit
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/atomic_audit.py" --selftest)
    # The catalog must REFUSE to answer from a directory that is not there,
    # rather than reporting "(no matches)" and exiting 0 -- that is the shape
    # that makes an anti-re-derivation tool actively harmful.
    add_test(NAME instrument_catalog_refuses_empty_corpus
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/catalog.py"
                --dir "${CMAKE_CURRENT_BINARY_DIR}/no-such-catalog" search anything)
    set_tests_properties(instrument_catalog_refuses_empty_corpus
        PROPERTIES WILL_FAIL TRUE)
endif()

# RtlTimeToTimeFields / RtlTimeFieldsToTime. Verified until now only by "the
# title reached the campaign after these landed", which shows they do not trap,
# not that they compute the right date. Anchored on external facts -- the 1601
# epoch, the 1970 delta, the weekdays of both, and the century leap rule -- so a
# wrong epoch constant or an off-by-one fails the test rather than merely
# producing a different plausible date.
add_executable(test_time_fields
    test_time_fields.cpp
    ../runtime/kernel_rtl.cpp
    ../runtime/guest_heap.cpp
    ../runtime/guest_memory.cpp)
target_include_directories(test_time_fields PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde"
    ../extern/XenonRecomp/XenonUtils)
target_link_libraries(test_time_fields PRIVATE lucent::lucent)
add_test(NAME time_fields COMMAND test_time_fields)
