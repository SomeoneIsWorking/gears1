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
    ../runtime/xam_notify.cpp
    ../runtime/xam_apps.cpp
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

add_executable(test_remote_input
    test_remote_input.cpp
    ../runtime/input.cpp)
target_include_directories(test_remote_input PRIVATE ../runtime)
target_link_libraries(test_remote_input PRIVATE lucent::lucent)
add_test(NAME remote_input COMMAND test_remote_input)

add_executable(test_graphics_probe
    test_graphics_probe.cpp
    ../runtime/graphics_probe.cpp)
target_include_directories(test_graphics_probe PRIVATE ../runtime)
add_test(NAME graphics_probe COMMAND test_graphics_probe)

add_executable(test_frame_probe_capture test_frame_probe_capture.cpp)
target_include_directories(test_frame_probe_capture PRIVATE ../runtime)
add_test(NAME frame_probe_capture COMMAND test_frame_probe_capture)

add_executable(test_render_retirement test_render_retirement.cpp)
target_include_directories(test_render_retirement PRIVATE ../runtime)
add_test(NAME render_retirement COMMAND test_render_retirement)

add_executable(test_gpu_retirement
    test_gpu_retirement.cpp
    ../runtime/gpu_retirement.cpp)
target_include_directories(test_gpu_retirement PRIVATE ../runtime)
add_test(NAME gpu_retirement COMMAND test_gpu_retirement)

add_executable(test_gpu_frame_timing test_gpu_frame_timing.cpp)
target_include_directories(test_gpu_frame_timing PRIVATE ../runtime)
add_test(NAME gpu_frame_timing COMMAND test_gpu_frame_timing)

add_executable(test_guest_write_watch
    test_guest_write_watch.cpp
    ../runtime/fault_report.cpp
    ../runtime/guest_write_watch.cpp
    ../runtime/guest_memory.cpp)
target_include_directories(test_guest_write_watch PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde")
target_link_libraries(test_guest_write_watch PRIVATE lucent::lucent)
add_test(NAME guest_write_watch COMMAND test_guest_write_watch)

add_executable(test_gpu_queue_access test_gpu_queue_access.cpp)
target_link_libraries(test_gpu_queue_access PRIVATE gears_gpu_queue_access)
add_test(NAME gpu_queue_access COMMAND test_gpu_queue_access)

add_executable(test_shared_frame_image
    test_shared_frame_image.cpp
    ../runtime/frame_contract.cpp
    ../runtime/gpu_shared_device.cpp)
target_include_directories(test_shared_frame_image PRIVATE ../runtime)
target_link_libraries(test_shared_frame_image PRIVATE lucent::lucent)
add_test(NAME shared_frame_image COMMAND test_shared_frame_image)

add_executable(test_gpu_draw_options
    test_gpu_draw_options.cpp
    ../runtime/gpu_draw_options.cpp)
target_include_directories(test_gpu_draw_options PRIVATE ../runtime)
target_link_libraries(test_gpu_draw_options PRIVATE lucent::lucent)
add_test(NAME gpu_draw_options COMMAND test_gpu_draw_options)

add_executable(test_gpu_draw_indices test_gpu_draw_indices.cpp)
target_include_directories(test_gpu_draw_indices PRIVATE ../runtime)
target_link_libraries(test_gpu_draw_indices PRIVATE gears_draw lucent::lucent)
add_test(NAME gpu_draw_indices COMMAND test_gpu_draw_indices)

add_executable(test_gpu_draw_untile test_gpu_draw_untile.cpp)
target_include_directories(test_gpu_draw_untile PRIVATE ../runtime)
target_link_libraries(test_gpu_draw_untile PRIVATE gears_draw lucent::lucent)
add_test(NAME gpu_draw_untile COMMAND test_gpu_draw_untile)

add_executable(test_title_profile test_title_profile.cpp)
target_link_libraries(test_title_profile PRIVATE gears_title_profile)
add_test(NAME title_profile COMMAND test_title_profile)

add_executable(test_generated_title_profile test_generated_title_profile.cpp)
target_include_directories(test_generated_title_profile PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}")
target_link_libraries(test_generated_title_profile PRIVATE gears_generated_title_profile)
add_test(NAME generated_title_profile COMMAND test_generated_title_profile)

add_executable(test_title_executable test_title_executable.cpp)
target_link_libraries(test_title_executable PRIVATE gears_title_executable)
add_test(NAME title_executable COMMAND test_title_executable)

add_executable(test_scanout_gamma
    test_scanout_gamma.cpp
    ../runtime/scanout_gamma.cpp)
target_include_directories(test_scanout_gamma PRIVATE ../runtime)
add_test(NAME scanout_gamma COMMAND test_scanout_gamma)

add_executable(test_depth_alias_shader_format test_depth_alias_shader_format.cpp)
target_include_directories(test_depth_alias_shader_format PRIVATE ../runtime)
add_test(NAME depth_alias_shader_format COMMAND test_depth_alias_shader_format)

# Xam system-UI notifications are part of the storage-selector protocol, not
# decorative events: Gears keeps its own modal open until it receives the close
# notification. Exercise the real queue through the imported guest seams.
add_executable(test_xam_notify
    test_xam_notify.cpp
    ../runtime/xam_notify.cpp
    ../runtime/xam_apps.cpp
    ../runtime/xam_overlapped.cpp
    ../runtime/kernel_objects.cpp
    ../runtime/guest_heap.cpp
    ../runtime/guest_memory.cpp
    ../runtime/guest_thread.cpp)
target_include_directories(test_xam_notify PRIVATE
    ../runtime
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde"
    ../extern/XenonRecomp/XenonUtils)
target_link_libraries(test_xam_notify PRIVATE lucent::lucent)
add_test(NAME xam_notify COMMAND test_xam_notify)

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
add_test(NAME python_project_locked
    COMMAND "${GEARS_UV_EXECUTABLE}" lock --check --project "${CMAKE_SOURCE_DIR}")
add_test(NAME source_structure
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tools/check_source_structure.py")
add_test(NAME source_structure_selftest
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tools/check_source_structure.py" --selftest)
add_test(NAME re_frontier_selftest
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tools/re_frontier.py" selftest)
add_test(NAME clean_distribution
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tools/check_distribution_clean.py")
add_test(NAME clean_distribution_selftest
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tools/check_distribution_clean.py" --selftest)
add_test(NAME title_identity
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_CURRENT_SOURCE_DIR}/test_title_identity.py")
add_test(NAME gdf_extract
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_CURRENT_SOURCE_DIR}/test_gdf_extract.py")
add_test(NAME title_conformance_selftest
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tools/title_conformance.py" --selftest)
add_test(NAME instrument_find_addr_refs
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/find_addr_refs.py" --selftest)
add_test(NAME instrument_atomic_audit
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/atomic_audit.py" --selftest)
# The catalog must REFUSE to answer from a directory that is not there,
# rather than reporting "(no matches)" and exiting 0 -- that is the shape
# that makes an anti-re-derivation tool actively harmful.
add_test(NAME instrument_catalog_refuses_empty_corpus
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/catalog.py"
            --dir "${CMAKE_CURRENT_BINARY_DIR}/no-such-catalog" search anything)
set_tests_properties(instrument_catalog_refuses_empty_corpus
    PROPERTIES WILL_FAIL TRUE)
# frame_stats.py reports catalog #62's headline number. Its self-test
# includes cases that must NOT trip -- the R/B-swap detector staying silent
# on an unrelated pair, an all-black frame being named as black rather than
# handed a ratio, and the PNG reader matching the PPM reader byte for byte,
# without which a cross-format diff compares two decoders.
add_test(NAME instrument_frame_stats
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/frame_stats.py" --selftest)
# ... and it must REFUSE a file that is not there rather than print
# statistics of nothing.
add_test(NAME instrument_frame_stats_refuses_missing
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/frame_stats.py"
            "${CMAKE_CURRENT_BINARY_DIR}/no-such-frame.ppm")
set_tests_properties(instrument_frame_stats_refuses_missing
    PROPERTIES WILL_FAIL TRUE)
# Same for the capture search: a search of a corpus that is not there
# searched NOTHING, and must not be readable as "no matches".
add_test(NAME instrument_find_const_block_refuses_missing
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/find_const_block.py"
            "${CMAKE_CURRENT_BINARY_DIR}/no-such-capture.gfr")
set_tests_properties(instrument_find_const_block_refuses_missing
    PROPERTIES WILL_FAIL TRUE)
# clip_volume_check.py carries every number inline, so it needs no capture:
# it re-runs catalog #74's arithmetic, requires it to agree with the GPU on
# the draw that rasterised, AND requires a deliberately wrong constant
# layout to be rejected. A check that has only ever passed is not a check.
add_test(NAME instrument_clip_volume_check
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/clip_volume_check.py")
# The pass classifier's own self-test: cases it must accept, and one it
# must reject.
add_test(NAME instrument_pass_structure
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/pass_structure.py" --selftest)
# gfr_to_xtr transcribes Xenia's trace format and Xenos packet bitfields by
# hand. A transcription slip there produces a trace that parses and renders
# the WRONG thing -- an oracle that lies is worse than no oracle -- so the
# packing is checked against values worked out independently.
add_test(NAME instrument_gfr_to_xtr
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/gfr_to_xtr.py" --selftest)
add_test(NAME instrument_gfr_to_xtr_refuses_missing
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/gfr_to_xtr.py"
            "${CMAKE_CURRENT_BINARY_DIR}/no-such.gfr" "${CMAKE_CURRENT_BINARY_DIR}/o.xtr")
set_tests_properties(instrument_gfr_to_xtr_refuses_missing
    PROPERTIES WILL_FAIL TRUE)
# The interval ledger must distinguish three cases before it is allowed to
# interpret a real capture: exact alignment, the same draw touching a
# different EDRAM owner, and a draw present on only one side.
add_test(NAME instrument_draw_interval_ledger
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tools/draw_interval_ledger.py" --selftest)
# Partial tiled oracle ranges are only comparable when their allocation
# base is proven. Exercise explicit metadata, unique legacy inference, the
# ambiguous refusal, an identical image and a deliberately shifted image.
add_test(NAME instrument_layer_compare
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tools/layer_compare.py" --selftest)
# A camera and input schedule can both match behind a title-owned modal.
# Exercise isolated clean draws, the measured consecutive eight-draw modal
# suffix, zero occurrences and an unknown run so the capture harness cannot
# silently accept unequal UI state or overfit a whole-frame draw count.
add_test(NAME instrument_ui_state
    COMMAND ${GEARS_UV_RUN} python
            "${CMAKE_SOURCE_DIR}/tools/ui_state_check.py" --selftest)

add_test(NAME cpp_quality
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/check_cpp_quality.py"
            "${CMAKE_BINARY_DIR}")
add_test(NAME cpp_quality_selftest
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/check_cpp_quality.py"
            --selftest)

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

# Capture runs may render many warm-up frames but report only the final one.
# Resolve artifacts from warm-up frames used to accumulate in the same directory,
# turning a single-frame structural comparison into an accidental 301-frame corpus.
add_executable(test_frame_artifact_policy test_frame_artifact_policy.cpp)
target_include_directories(test_frame_artifact_policy PRIVATE ../runtime)
add_test(NAME frame_artifact_policy COMMAND test_frame_artifact_policy)

# Persistent EDRAM images are capacities, not a declaration that every frame
# must have the same active sample grid. Shrinking used to destroy an image
# while the window presenter was still blitting it.
add_executable(test_gpu_extent_capacity test_gpu_extent_capacity.cpp)
target_include_directories(test_gpu_extent_capacity PRIVATE ../runtime)
add_test(NAME gpu_extent_capacity COMMAND test_gpu_extent_capacity)

add_executable(test_gpu_surface_format_capacity test_gpu_surface_format_capacity.cpp)
target_include_directories(test_gpu_surface_format_capacity PRIVATE ../runtime)
target_link_libraries(test_gpu_surface_format_capacity PRIVATE gears_draw lucent::lucent)
add_test(NAME gpu_surface_format_capacity COMMAND test_gpu_surface_format_capacity)

add_executable(test_gpu_draw_sample_layout test_gpu_draw_sample_layout.cpp)
target_include_directories(test_gpu_draw_sample_layout PRIVATE ../runtime)
add_test(NAME gpu_draw_sample_layout COMMAND test_gpu_draw_sample_layout)

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
add_executable(test_guest_texture_hash test_guest_texture_hash.cpp)
target_link_libraries(test_guest_texture_hash PRIVATE gears_guest_texture_hash)
add_test(NAME guest_texture_hash COMMAND test_guest_texture_hash)

add_executable(test_guest_dirty_pages test_guest_dirty_pages.cpp)
target_link_libraries(test_guest_dirty_pages PRIVATE gears_draw lucent::lucent)
add_test(NAME guest_dirty_pages COMMAND test_guest_dirty_pages)

add_executable(test_gpu_resolve_extent
    test_gpu_resolve_extent.cpp
    ../runtime/gpu_resolve_extent.cpp)
target_include_directories(test_gpu_resolve_extent PRIVATE ../runtime)
add_test(NAME gpu_resolve_extent COMMAND test_gpu_resolve_extent)

if(TARGET gears_draw_xlate)
    add_executable(test_guest_texture_decode test_guest_texture_decode.cpp)
    target_include_directories(test_guest_texture_decode PRIVATE
        ../runtime "${CMAKE_SOURCE_DIR}/extern/xenia/src")
    target_compile_definitions(test_guest_texture_decode PRIVATE
        GEARS_HAVE_GUEST_DRAW)
    target_compile_options(test_guest_texture_decode PRIVATE -mavx)
    target_link_libraries(test_guest_texture_decode PRIVATE gears_draw_xlate)
    add_test(NAME guest_texture_decode COMMAND test_guest_texture_decode)
endif()

# The swapchain format choice. It decides every pixel of the window and NO capture
# in this project can see it: the renderer's screenshots come from its own readback,
# before the blit into the swapchain. An sRGB swapchain re-encodes an
# already-tonemapped frame, so the window washes out while every file on disk looks
# correct. Needs only the Vulkan headers.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    add_executable(test_swapchain_format test_swapchain_format.cpp)
    target_include_directories(test_swapchain_format PRIVATE ../runtime ${Vulkan_INCLUDE_DIRS})
    add_test(NAME swapchain_format COMMAND test_swapchain_format)

endif()

# The audio pump's pacing policy. What a pump does with slots it MISSED decides
# what the game sounds like: serving them back to back makes the title consume its
# stream faster than real time (a raised pitch that lasts as long as the deficit),
# while dropping them keeps the stream in real time at the cost of a gap. The
# policy is a pure function of two time points so it can be tested without a
# clock, a thread or a guest.
add_executable(test_audio_pace test_audio_pace.cpp)
target_include_directories(test_audio_pace PRIVATE ../runtime)
add_test(NAME audio_pace COMMAND test_audio_pace)

# The render-driver frame's layout: the guest hands us six PLANES of 256
# big-endian floats and every host API wants them interleaved. Reading one as the
# other does not fail, it plays -- far too high, with a click at every plane
# crossing -- so the test builds a frame whose planes hold per-channel constants,
# which the two readings cannot both satisfy.
add_executable(test_audio_frame test_audio_frame.cpp)
target_include_directories(test_audio_frame PRIVATE ../runtime)
add_test(NAME audio_frame COMMAND test_audio_frame)

# The in-run A/B. Comparing two runs of the title does not resolve anything below
# ~10 ms a frame: three runs of the same gameplay walk at an identical 743 draws a
# frame gave 39.4, 47.2 and 42.7 ms draw loops, and the two SLOWER ones were the
# runs with 3.6 ms of work removed. The arms therefore alternate frame by frame
# inside one run, and the summary is required to say when it cannot tell them
# apart -- a bare difference of means reads as a finding however small it is.
add_executable(test_frame_ab
    test_frame_ab.cpp
    ../runtime/frame_ab.cpp)
target_include_directories(test_frame_ab PRIVATE ../runtime)
add_test(NAME frame_ab COMMAND test_frame_ab)

# The live renderer's producer must never wait behind Vulkan work. It owns one
# pending frame, replaces it with a newer arrival, rejects stale IDs, and drains
# deterministically when closed. This test exercises the production queue with
# no renderer, clock, guest, or graphics device.
add_executable(test_frame_queue
    test_frame_queue.cpp
    ../runtime/frame_queue.cpp)
target_include_directories(test_frame_queue PRIVATE ../runtime)
add_test(NAME frame_queue COMMAND test_frame_queue)

add_executable(test_frame_contract
    test_frame_contract.cpp
    ../runtime/frame_contract.cpp)
target_include_directories(test_frame_contract PRIVATE ../runtime)
add_test(NAME frame_contract COMMAND test_frame_contract)

add_executable(test_rhi_semantic_stream
    test_rhi_semantic_stream.cpp
    ../runtime/rhi_semantic_stream.cpp)
target_include_directories(test_rhi_semantic_stream PRIVATE ../runtime)
target_link_libraries(test_rhi_semantic_stream PRIVATE lucent::lucent)
add_test(NAME rhi_semantic_stream COMMAND test_rhi_semantic_stream)

add_executable(test_gpu_swap_packet
    test_gpu_swap_packet.cpp
    ../runtime/gpu_swap_packet.cpp
    ../runtime/rhi_semantic_stream.cpp)
target_include_directories(test_gpu_swap_packet PRIVATE ../runtime)
target_link_libraries(test_gpu_swap_packet PRIVATE lucent::lucent)
add_test(NAME gpu_swap_packet COMMAND test_gpu_swap_packet)

add_executable(test_shader_setter_state test_shader_setter_state.cpp)
target_include_directories(test_shader_setter_state PRIVATE ../runtime)
add_test(NAME shader_setter_state COMMAND test_shader_setter_state)

add_executable(test_gpu_draw_census
    test_gpu_draw_census.cpp
    ../runtime/gpu_draw_census.cpp)
target_include_directories(test_gpu_draw_census PRIVATE ../runtime)
target_link_libraries(test_gpu_draw_census PRIVATE lucent::lucent)
add_test(NAME gpu_draw_census COMMAND test_gpu_draw_census)

add_executable(test_gpu_surface_target_lookup test_gpu_surface_target_lookup.cpp)
target_include_directories(test_gpu_surface_target_lookup PRIVATE ../runtime)
target_link_libraries(test_gpu_surface_target_lookup PRIVATE lucent::lucent)
add_test(NAME gpu_surface_target_lookup COMMAND test_gpu_surface_target_lookup)

# The fragment-output clamp. A fixed-point render target clamps a shader's colour
# output to [0,1] before blending and a floating-point one does not -- and this
# renderer gives a reinterpreted surface ONE float host image, so every draw into
# it behaves as if its target were HDR. The visible cost is a translucent highlight
# blending as a flat saturated slab (`catalog.py show 68`). Pure SPIR-V surgery, so
# the test needs no Vulkan device: it checks that a clamp is inserted before each
# colour store, that the module stays well-formed, and above all that a module the
# transform cannot handle is REFUSED rather than left half-rewritten.
add_executable(test_spirv_clamp test_spirv_clamp.cpp ../runtime/spirv_clamp.cpp)
target_include_directories(test_spirv_clamp PRIVATE ../runtime)
add_test(NAME spirv_clamp COMMAND test_spirv_clamp)
