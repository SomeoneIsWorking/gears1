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
    ../runtime/guest_stack_argument.cpp
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


# The fault report's address classifier. The handler itself runs in a process
# that is already broken and cannot be unit-tested; the classification of the
# faulting address can be, and it is the part that decides whether the report
# says anything useful. The low-address case is the one that matters most: this
# runtime maps guest physical zero as real RAM, so a fault there is NOT a guest
# null pointer and reporting it as one would send the next session hunting a bug
# that does not exist.
add_executable(test_fault_report
    test_fault_report.cpp
    ../runtime/fault_report.cpp)
target_include_directories(test_fault_report PRIVATE ../runtime)
target_link_libraries(test_fault_report PRIVATE lucent::lucent)
add_test(NAME fault_report COMMAND test_fault_report)

# The indirect-call target check. The bounds come from the generated ppc_config.h
# so the test pins the REAL code geometry -- a regenerated image with a different
# code range must not leave this asserting yesterday's addresses. The garbage
# cases are the actual values seen in the #45 crash, each of which the unchecked
# macro would have indexed into the function table and called.
add_executable(test_guest_indirect_call test_guest_indirect_call.cpp)
target_include_directories(test_guest_indirect_call PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde"
    ../extern/XenonRecomp/XenonUtils)
add_test(NAME guest_indirect_call COMMAND test_guest_indirect_call)

# NtCreateFile's CreateDisposition, which the runtime ignored entirely -- it never
# read the argument, and answered a read-only open of a missing file with a newly
# created empty one and STATUS_SUCCESS. Written before the fix. The dispositions
# here are NT's, not the console's XCONTENT ones, which use different numbers for
# different meanings.
add_executable(test_file_disposition
    test_file_disposition.cpp
    ../runtime/file_disposition.cpp)
target_include_directories(test_file_disposition PRIVATE ../runtime)
add_test(NAME file_disposition COMMAND test_file_disposition)

# Reading guest arguments passed on the STACK. XamContentCreateEx takes nine
# parameters and our implementation documented seven, reading the overlapped block
# out of r9 -- which is cacheSize, and which the title's own wrapper zeroes after
# spilling the real overlapped to r1+84. The decoy case is the point of the file.
add_executable(test_guest_stack_argument
    test_guest_stack_argument.cpp
    ../runtime/guest_stack_argument.cpp)
target_include_directories(test_guest_stack_argument PRIVATE ../runtime)
add_test(NAME guest_stack_argument COMMAND test_guest_stack_argument)

# Vulkan queue-family selection, factored out of gpu_draw.cpp and gpu_present.cpp
# ahead of unifying their two devices. Each file's inline loop checked only its own
# half of the requirement -- graphics for one, presentation for the other -- so the
# case that matters here is a device where the first graphics family cannot present.
# "None" is a real answer: returning family 0 when nothing qualifies is how a port
# ends up with a device that silently cannot present.
add_executable(test_queue_family
    test_queue_family.cpp
    ../runtime/gpu_queue_family.cpp)
target_include_directories(test_queue_family PRIVATE ../runtime)
add_test(NAME queue_family COMMAND test_queue_family)

# The shared-VkInstance extension bookkeeping. Two devices cost a readback and a
# staging upload of every frame; unifying them starts with the instance, since a
# surface from one instance cannot be used with a device from another. The guarded
# failure is the quiet one: handing a later caller an instance lacking what it asked
# for, so the error surfaces at surface creation instead of at the cause.
add_executable(test_gpu_instance
    test_gpu_instance.cpp
    ../runtime/gpu_instance.cpp)
target_include_directories(test_gpu_instance PRIVATE ../runtime)
add_test(NAME gpu_instance COMMAND test_gpu_instance)

# Guest-texture change detection. The renderer caches uploads against the fetch
# constant, which does not change when the guest overwrites the pixels at the same
# address -- a render target, an animated UI element or a streamed mip all do that.
# The property under test is COVERAGE: a hash over a few sampled offsets would pass
# a naive test and silently miss an interior change, leaving a stale texture on
# screen with no error anywhere, so the test flips every byte position in turn.
add_executable(test_guest_texture_hash
    test_guest_texture_hash.cpp
    ../runtime/guest_texture_hash.cpp)
target_include_directories(test_guest_texture_hash PRIVATE ../runtime)
add_test(NAME guest_texture_hash COMMAND test_guest_texture_hash)
