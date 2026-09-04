# The gameplay target is intentionally absent until shared/x360port embeds
# Xenia's Xenon dynarec. Keeping this named target makes direct build attempts
# fail at the same explicit boundary as the launcher, without preserving a
# generated-code bridge.
add_custom_target(gears1
    COMMAND ${GEARS_UV_RUN} python "${CMAKE_SOURCE_DIR}/tools/require_x360port.py"
    COMMENT "Checking the GearsUE3 gameplay executor boundary"
    VERBATIM)

set(GEARS_HOST_OPT "-O2" CACHE STRING "Optimisation level for host-native code")
function(gears_optimise_host target)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:${GEARS_HOST_OPT}>)
    endif()
endfunction()

add_library(gears_title_profile STATIC title_profile.cpp)
target_include_directories(gears_title_profile PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(gears_gpu_queue_access STATIC gpu_queue_access.cpp)
target_include_directories(gears_gpu_queue_access PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# The watchdog is retained native diagnostic infrastructure. Building it here
# keeps its lock-free guest-call path checked while execution wiring waits for
# x360port; it does not create or call a guest executor.
add_library(gears_wait_probe STATIC wait_probe.cpp)
target_include_directories(gears_wait_probe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gears_wait_probe PRIVATE lucent::lucent)

if(TARGET xxHash::xxhash)
    add_library(gears_guest_texture_hash STATIC guest_texture_hash.cpp)
    target_include_directories(gears_guest_texture_hash PUBLIC .
        PRIVATE "${GEARS_XXHASH_ROOT}")
    target_link_libraries(gears_guest_texture_hash PRIVATE xxHash::xxhash)
endif()

# The retained renderer and semantic-native seams are executor-independent.
# They remain buildable as focused libraries and offline instruments while the
# product waits for x360port; no target here executes guest PPC.
if(TARGET gears_guest_texture_hash)
    add_library(gears_draw STATIC
        frame_ab.cpp
        frame_capture.cpp
        frame_content.cpp
        frame_contract.cpp
        gpu_device_features.cpp
        gpu_draw_ab.cpp
        gpu_draw.cpp
        gpu_draw_api.cpp
        gpu_draw_arena.cpp
        gpu_draw_census.cpp
        gpu_draw_descriptors.cpp
        gpu_draw_formats.cpp
        gpu_draw_indices.cpp
        gpu_draw_native_input.cpp
        gpu_draw_options.cpp
        gpu_draw_pipelines.cpp
        gpu_draw_pixels.cpp
        gpu_draw_probe.cpp
        gpu_draw_reinterpret.cpp
        gpu_draw_resolve.cpp
        gpu_draw_resolve_decode.cpp
        gpu_draw_resolve_plan.cpp
        gpu_draw_shader_override.cpp
        gpu_draw_shaders.cpp
        gpu_draw_targets.cpp
        gpu_draw_textures.cpp
        gpu_draw_uniforms.cpp
        gpu_draw_untile.cpp
        gpu_draw_vertexfetch.cpp
        gpu_frame_cleanup.cpp
        gpu_frame_slots.cpp
        gpu_frame_timing.cpp
        gpu_instance.cpp
        gpu_queue_family.cpp
        gpu_retirement.cpp
        gpu_renderer_capacity.cpp
        gpu_renderer_lifetime.cpp
        gpu_resolve_extent.cpp
        gpu_scanout.cpp
        gpu_scanout_gamma.cpp
        gpu_shared_device.cpp
        guest_dirty_pages.cpp
        native_pass.cpp
        render_ab.cpp
        scanout_gamma.cpp
        spirv_clamp.cpp)
    target_include_directories(gears_draw PUBLIC . PRIVATE "${GEARS_XXHASH_ROOT}")
    target_link_libraries(gears_draw PRIVATE lucent::lucent gears_gpu_queue_access
                                             gears_guest_texture_hash)

    add_library(gears1_native_pass STATIC titles/gears1/native_pass.cpp)
    target_include_directories(gears1_native_pass PUBLIC .)
    add_library(gears1_draw INTERFACE)
    target_link_libraries(gears1_draw INTERFACE gears_draw gears1_native_pass)

    find_package(Vulkan QUIET)
    if(Vulkan_FOUND AND GEARS_HAVE_XENOS_TRANSLATOR)
        add_library(gears_draw_xlate STATIC
            gpu_draw_depth_bias.cpp
            gpu_draw_point_geometry.cpp
            gpu_draw_shader_interface.cpp
            gpu_draw_texture_decode.cpp
            gpu_draw_xlate.cpp)
        target_compile_definitions(gears_draw_xlate PRIVATE GEARS_HAVE_GUEST_DRAW)
        target_include_directories(gears_draw_xlate PRIVATE .)
        target_link_libraries(gears_draw_xlate PRIVATE xenia_gpu lucent::lucent)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
            target_compile_options(gears_draw_xlate PRIVATE -mavx)
        endif()
        target_compile_definitions(gears_draw PRIVATE GEARS_HAVE_GUEST_DRAW)
        target_link_libraries(gears_draw PUBLIC Vulkan::Vulkan PRIVATE gears_draw_xlate)

        add_executable(frame_replay "${CMAKE_CURRENT_SOURCE_DIR}/../tools/frame_replay/main.cpp")
        target_link_libraries(frame_replay PRIVATE gears1_draw lucent::lucent)
        message(STATUS "gears1: executor-independent frame_replay enabled")
    endif()
endif()

foreach(gears_host_target
        gears_title_profile gears_gpu_queue_access gears_wait_probe gears_xxhash
        gears_guest_texture_hash gears_draw gears1_native_pass gears_draw_xlate
        frame_replay)
    gears_optimise_host(${gears_host_target})
endforeach()
