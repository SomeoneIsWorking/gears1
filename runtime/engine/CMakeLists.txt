# GearsUE3's native engine. Everything here is independently authored C++ that
# operates on the title's own cooked content; nothing executes guest code.

add_library(gears_engine_package STATIC
    package/byte_reader.cpp
    package/compressed_record.cpp
    package/content_files.cpp
    package/lzo1x.cpp
    package/object_tables.cpp
    package/package.cpp
    package/package_store.cpp
    package/package_summary.cpp)
target_include_directories(gears_engine_package PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
x360port_enable_warnings(gears_engine_package)

add_library(gears_engine_object STATIC
    object/bulk_data.cpp
    object/class_hierarchy.cpp
    object/object_resolver.cpp
    object/property_values.cpp
    object/serialized_object.cpp
    object/tagged_properties.cpp)
target_link_libraries(gears_engine_object PUBLIC gears_engine_package)
x360port_enable_warnings(gears_engine_object)

add_library(gears_engine_texture STATIC
    texture/texture2d.cpp
    texture/xenos_tiling.cpp)
target_link_libraries(gears_engine_texture PUBLIC gears_engine_object)
x360port_enable_warnings(gears_engine_texture)

add_library(gears_engine_mesh STATIC
    mesh/static_mesh.cpp)
target_link_libraries(gears_engine_mesh PUBLIC gears_engine_object)
x360port_enable_warnings(gears_engine_mesh)

add_library(gears_engine_bsp STATIC
    bsp/bsp_geometry.cpp
    bsp/bsp_model.cpp
    bsp/component_geometry.cpp
    bsp/model_component.cpp)
target_link_libraries(gears_engine_bsp PUBLIC gears_engine_mesh)
x360port_enable_warnings(gears_engine_bsp)

add_library(gears_engine_material STATIC
    material/material_textures.cpp)
target_link_libraries(gears_engine_material PUBLIC gears_engine_object)
x360port_enable_warnings(gears_engine_material)

add_library(gears_engine_scene STATIC
    scene/camera.cpp
    scene/level_scene.cpp
    scene/streaming_levels.cpp
    scene/transform.cpp)
target_link_libraries(gears_engine_scene PUBLIC gears_engine_bsp)
x360port_enable_warnings(gears_engine_scene)

add_executable(test_engine_package "${CMAKE_SOURCE_DIR}/tests/test_engine_package.cpp")
target_link_libraries(test_engine_package PRIVATE gears_engine_package)
target_include_directories(test_engine_package PRIVATE "${CMAKE_SOURCE_DIR}/tests")
x360port_enable_warnings(test_engine_package)
gears_add_binary_test(gears_engine_package test_engine_package)

add_executable(test_engine_object "${CMAKE_SOURCE_DIR}/tests/test_engine_object.cpp")
target_link_libraries(test_engine_object PRIVATE gears_engine_object)
target_include_directories(test_engine_object PRIVATE "${CMAKE_SOURCE_DIR}/tests")
x360port_enable_warnings(test_engine_object)
gears_add_binary_test(gears_engine_object test_engine_object)

add_executable(test_engine_bsp "${CMAKE_SOURCE_DIR}/tests/test_engine_bsp.cpp")
target_link_libraries(test_engine_bsp PRIVATE gears_engine_bsp)
x360port_enable_warnings(test_engine_bsp)
gears_add_binary_test(gears_engine_bsp test_engine_bsp)

add_executable(test_engine_scene "${CMAKE_SOURCE_DIR}/tests/test_engine_scene.cpp")
target_link_libraries(test_engine_scene PRIVATE gears_engine_scene)
x360port_enable_warnings(test_engine_scene)
gears_add_binary_test(gears_engine_scene test_engine_scene)

# Loads every cooked package of an operator-supplied extracted disc. Not a
# CTest: restricted title bytes never become a hosted-test input.
add_executable(gears_package_census "${CMAKE_SOURCE_DIR}/tools/package_census/main.cpp")
target_link_libraries(gears_package_census PRIVATE gears_engine_texture gears_engine_mesh
                                                   gears_engine_material gears_engine_bsp
                                                   lucent::lucent)
x360port_enable_warnings(gears_package_census)

# Prints one package's exports or the leading bytes of one class's exports.
add_executable(gears_package_inspect "${CMAKE_SOURCE_DIR}/tools/package_inspect/main.cpp")
target_link_libraries(gears_package_inspect PRIVATE gears_engine_object lucent::lucent)
x360port_enable_warnings(gears_package_inspect)

# Writes decoded textures of one package as DDS files for inspection.
add_executable(gears_texture_export "${CMAKE_SOURCE_DIR}/tools/texture_export/main.cpp")
target_link_libraries(gears_texture_export PRIVATE gears_engine_texture lucent::lucent)
x360port_enable_warnings(gears_texture_export)

# The native renderer. Optional like the other Vulkan consumers: without the
# Vulkan loader and glslc the engine's data layers still build and test.
find_package(Vulkan QUIET COMPONENTS glslc)
if(Vulkan_FOUND AND Vulkan_glslc_FOUND)
    set(GEARS_ENGINE_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/render/spirv")
    set(GEARS_ENGINE_SHADER_HEADERS "")
    foreach(stage vert frag)
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/mesh.${stage}")
        set(binary "${GEARS_ENGINE_SHADER_DIR}/mesh_${stage}.spv")
        set(header "${GEARS_ENGINE_SHADER_DIR}/mesh_${stage}.h")
        add_custom_command(
            OUTPUT "${header}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${GEARS_ENGINE_SHADER_DIR}"
            COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.1 -O -o "${binary}"
                    "${source}"
            COMMAND "${CMAKE_COMMAND}" -DSPIRV=${binary} -DHEADER=${header}
                    -DSYMBOL=kMesh_${stage}
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/render/embed_spirv.cmake"
            DEPENDS "${source}" "${CMAKE_CURRENT_SOURCE_DIR}/render/embed_spirv.cmake"
            COMMENT "Compiling engine shader mesh.${stage}"
            VERBATIM)
        list(APPEND GEARS_ENGINE_SHADER_HEADERS "${header}")
    endforeach()
    add_custom_target(gears_engine_shaders DEPENDS ${GEARS_ENGINE_SHADER_HEADERS})

    add_library(gears_engine_render STATIC
        render/device_image.cpp
        render/gpu_texture.cpp
        render/level_renderer.cpp
        render/mesh_renderer.cpp
        render/offscreen_target.cpp
        render/texture_bindings.cpp
        render/vulkan_device.cpp)
    add_dependencies(gears_engine_render gears_engine_shaders)
    target_include_directories(gears_engine_render PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/render")
    target_link_libraries(gears_engine_render PUBLIC gears_engine_scene gears_engine_material
                                              gears_engine_texture Vulkan::Vulkan)
    x360port_enable_warnings(gears_engine_render)

    # Renders one level package headlessly and writes the frame as an image.
    add_executable(gears_level_render "${CMAKE_SOURCE_DIR}/tools/level_render/main.cpp")
    target_link_libraries(gears_level_render PRIVATE gears_engine_render lucent::lucent)
    x360port_enable_warnings(gears_level_render)
else()
    message(STATUS "GearsUE3 engine renderer: disabled (needs the Vulkan loader and glslc)")
endif()
