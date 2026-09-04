# Only executor-independent contracts are active during the break-first
# migration. Tests that exercised the generated PPC ABI were removed with that
# ABI; retained host semantics remain in focused native tests.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU|AppleClang")
    add_compile_options(-UNDEBUG)
elseif(MSVC)
    add_compile_options(/UNDEBUG)
endif()

add_executable(test_title_profile test_title_profile.cpp)
target_link_libraries(test_title_profile PRIVATE gears_title_profile)
add_test(NAME title_profile COMMAND test_title_profile)

add_executable(test_rhi_resource_reference
    test_rhi_resource_reference.cpp
    ../runtime/rhi_resource_reference.cpp)
target_include_directories(test_rhi_resource_reference PRIVATE
    ../runtime ../runtime/titles/gears1)
add_test(NAME rhi_resource_reference COMMAND test_rhi_resource_reference)

add_executable(test_gears1_native_pass test_gears1_native_pass.cpp ../runtime/native_pass.cpp)
target_include_directories(test_gears1_native_pass PRIVATE ../runtime)
target_link_libraries(test_gears1_native_pass PRIVATE gears1_native_pass lucent::lucent)
add_test(NAME gears1_native_pass_disabled COMMAND test_gears1_native_pass)
add_test(NAME gears1_native_pass_enabled COMMAND test_gears1_native_pass)
set_tests_properties(gears1_native_pass_enabled PROPERTIES ENVIRONMENT "GEARS_NATIVE_PASSES=1")

add_executable(test_gpu_queue_access test_gpu_queue_access.cpp)
target_link_libraries(test_gpu_queue_access PRIVATE gears_gpu_queue_access)
add_test(NAME gpu_queue_access COMMAND test_gpu_queue_access)

add_executable(test_gpu_draw_options test_gpu_draw_options.cpp ../runtime/gpu_draw_options.cpp)
target_include_directories(test_gpu_draw_options PRIVATE ../runtime)
target_link_libraries(test_gpu_draw_options PRIVATE lucent::lucent)
add_test(NAME gpu_draw_options COMMAND test_gpu_draw_options)

if(TARGET gears1_draw)
    add_executable(test_gpu_draw_indices test_gpu_draw_indices.cpp)
    target_include_directories(test_gpu_draw_indices PRIVATE ../runtime)
    target_link_libraries(test_gpu_draw_indices PRIVATE gears1_draw lucent::lucent)
    add_test(NAME gpu_draw_indices COMMAND test_gpu_draw_indices)

    add_executable(test_gpu_draw_untile test_gpu_draw_untile.cpp)
    target_include_directories(test_gpu_draw_untile PRIVATE ../runtime)
    target_link_libraries(test_gpu_draw_untile PRIVATE gears1_draw lucent::lucent)
    add_test(NAME gpu_draw_untile COMMAND test_gpu_draw_untile)

    add_executable(test_guest_dirty_pages test_guest_dirty_pages.cpp)
    target_link_libraries(test_guest_dirty_pages PRIVATE gears1_draw lucent::lucent)
    add_test(NAME guest_dirty_pages COMMAND test_guest_dirty_pages)
endif()

if(TARGET gears_guest_texture_hash)
    add_executable(test_guest_texture_hash test_guest_texture_hash.cpp)
    target_link_libraries(test_guest_texture_hash PRIVATE gears_guest_texture_hash)
    add_test(NAME guest_texture_hash COMMAND test_guest_texture_hash)
endif()

add_executable(test_audio_pace test_audio_pace.cpp)
target_include_directories(test_audio_pace PRIVATE ../runtime)
add_test(NAME audio_pace COMMAND test_audio_pace)

add_executable(test_audio_frame test_audio_frame.cpp)
target_include_directories(test_audio_frame PRIVATE ../runtime)
add_test(NAME audio_frame COMMAND test_audio_frame)

# Native trace replayers remain build-checked without a guest executor. They
# consume only executor-independent memory and subsystem contracts.
add_executable(heap_replay
    ../tools/heap_replay.cpp
    ../runtime/guest_heap.cpp
    ../runtime/guest_memory.cpp)
target_include_directories(heap_replay PRIVATE ../runtime)
target_link_libraries(heap_replay PRIVATE lucent::lucent)

if(GEARS_HAVE_XMA_DECODER)
    add_executable(xma_replay
        ../tools/xma_replay.cpp
        ../runtime/xma_context.cpp
        ../runtime/xma_decode.cpp
        ../runtime/guest_memory.cpp)
    target_include_directories(xma_replay PRIVATE ../runtime)
    target_compile_definitions(xma_replay PRIVATE GEARS_HAVE_XMA_DECODER=1)
    target_link_libraries(xma_replay PRIVATE ffmpeg_xma lucent::lucent)
endif()

add_executable(test_frame_queue test_frame_queue.cpp ../runtime/frame_queue.cpp)
target_include_directories(test_frame_queue PRIVATE ../runtime)
add_test(NAME frame_queue COMMAND test_frame_queue)

add_executable(test_frame_contract test_frame_contract.cpp ../runtime/frame_contract.cpp)
target_include_directories(test_frame_contract PRIVATE ../runtime)
add_test(NAME frame_contract COMMAND test_frame_contract)

add_executable(test_rhi_semantic_stream
    test_rhi_semantic_stream.cpp
    rhi_renderer_input_origin_test.cpp
    rhi_pm4_shader_evidence_test.cpp
    ../runtime/rhi_pm4_shader_evidence.cpp
    ../runtime/rhi_renderer_input.cpp
    ../runtime/rhi_renderer_report_diagnostics.cpp
    ../runtime/rhi_semantic_state.cpp
    ../runtime/rhi_semantic_stream.cpp)
target_include_directories(test_rhi_semantic_stream PRIVATE ../runtime)
target_link_libraries(test_rhi_semantic_stream PRIVATE lucent::lucent)
add_test(NAME rhi_semantic_stream COMMAND test_rhi_semantic_stream)

add_executable(test_rhi_semantic_state test_rhi_semantic_state.cpp ../runtime/rhi_semantic_state.cpp)
target_include_directories(test_rhi_semantic_state PRIVATE ../runtime)
add_test(NAME rhi_semantic_state COMMAND test_rhi_semantic_state)

add_executable(test_native_rhi
    test_native_rhi.cpp
    ../runtime/native_rhi.cpp
    ../runtime/native_rhi_backend.cpp
    ../runtime/native_rhi_resources.cpp)
target_include_directories(test_native_rhi PRIVATE ../runtime)
target_link_libraries(test_native_rhi PRIVATE lucent::lucent)
add_test(NAME native_rhi COMMAND test_native_rhi)

add_executable(test_rhi_index_buffer
    test_rhi_index_buffer.cpp ../runtime/titles/gears1/rhi_index_buffer.cpp)
target_include_directories(test_rhi_index_buffer PRIVATE ../runtime)
add_test(NAME rhi_index_buffer COMMAND test_rhi_index_buffer)

add_executable(test_rhi_vertex_buffer
    test_rhi_vertex_buffer.cpp ../runtime/titles/gears1/rhi_vertex_buffer.cpp)
target_include_directories(test_rhi_vertex_buffer PRIVATE ../runtime)
add_test(NAME rhi_vertex_buffer COMMAND test_rhi_vertex_buffer)

add_executable(test_rhi_resolve
    test_rhi_resolve.cpp ../runtime/gpu_draw_formats.cpp ../runtime/titles/gears1/rhi_resolve.cpp)
target_include_directories(test_rhi_resolve PRIVATE ../runtime)
target_link_libraries(test_rhi_resolve PRIVATE lucent::lucent)
add_test(NAME rhi_resolve COMMAND test_rhi_resolve)

add_executable(test_spirv_clamp test_spirv_clamp.cpp ../runtime/spirv_clamp.cpp)
target_include_directories(test_spirv_clamp PRIVATE ../runtime)
add_test(NAME spirv_clamp COMMAND test_spirv_clamp)

# Migration and quality gates stay active even while the product executor is
# deliberately absent. They exercise the shipping Python paths and only the
# first-party C++ translation units present in this executor-independent graph.
add_test(NAME python_project_locked
    COMMAND "${GEARS_UV_EXECUTABLE}" lock --check --project "${CMAKE_SOURCE_DIR}")
add_test(NAME source_structure
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/check_source_structure.py")
add_test(NAME source_structure_selftest
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/check_source_structure.py" --selftest)
add_test(NAME project_info_entrypoint
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/info.py" brief x360port)
add_test(NAME re_frontier_selftest
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/re_frontier.py" selftest)
add_test(NAME clean_distribution
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/check_distribution_clean.py")
add_test(NAME clean_distribution_selftest
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/check_distribution_clean.py" --selftest)
add_test(NAME migration_boundary
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/check_migration_boundary.py")
add_test(NAME migration_boundary_selftest
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/check_migration_boundary.py" --selftest)
add_test(NAME title_identity
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_CURRENT_SOURCE_DIR}/test_title_identity.py")
add_test(NAME archive_input
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_CURRENT_SOURCE_DIR}/test_archive_input.py")
add_test(NAME gdf_extract
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_CURRENT_SOURCE_DIR}/test_gdf_extract.py")
add_test(NAME bootstrap_profile
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_CURRENT_SOURCE_DIR}/test_bootstrap_profile.py")
add_test(NAME bootstrap
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_CURRENT_SOURCE_DIR}/test_bootstrap.py")
add_test(NAME cpp_quality_selftest
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/check_cpp_quality.py" --selftest)
add_test(NAME cpp_quality
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/check_cpp_quality.py"
            "${CMAKE_BINARY_DIR}")
