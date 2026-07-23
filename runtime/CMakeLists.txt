# The translated game code. Gitignored and machine-local: it is produced from
# the user's own XEX by XenonRecomp.
file(GLOB PPC_RECOMP_SOURCES CONFIGURE_DEPENDS "${GEARS_PPC_DIR}/ppc_recomp.*.cpp")

if(NOT PPC_RECOMP_SOURCES)
    message(FATAL_ERROR
        "No generated sources in ${GEARS_PPC_DIR}. Run XenonRecomp first:\n"
        "  XenonRecomp config/gears.toml extern/XenonRecomp/XenonUtils/ppc_context.h")
endif()

list(LENGTH PPC_RECOMP_SOURCES PPC_RECOMP_COUNT)
message(STATUS "gears1: ${PPC_RECOMP_COUNT} recompiled translation units")

add_library(gears_ppc STATIC
    ${PPC_RECOMP_SOURCES}
    "${GEARS_PPC_DIR}/ppc_func_mapping.cpp")

target_include_directories(gears_ppc PUBLIC
    "${GEARS_PPC_DIR}"
    "${THIRDPARTY_ROOT}/simde")

# 49k functions of straight-line translated code; -O0 keeps build times sane
# while the runtime is still being brought up.
target_compile_options(gears_ppc PRIVATE -Wno-unused-variable)

# Import stubs are generated because the import list comes from the game.
set(IMPORT_STUBS "${CMAKE_CURRENT_BINARY_DIR}/imports_generated.cpp")
add_custom_command(
    OUTPUT "${IMPORT_STUBS}"
    COMMAND ${CMAKE_COMMAND} -E env python3
            "${CMAKE_SOURCE_DIR}/tools/gen_import_stubs.py"
            "${GEARS_PPC_DIR}/ppc_func_mapping.cpp"
            "${IMPORT_STUBS}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_import_stubs.py"
            "${GEARS_PPC_DIR}/ppc_func_mapping.cpp"
    COMMENT "Generating Xbox 360 kernel import stubs")

add_executable(gears1
    main.cpp
    guest_memory.cpp
    import_stub.cpp
    import_variables.cpp
    guest_heap.cpp
    guest_thread.cpp
    kernel_memory.cpp
    kernel_sync.cpp
    crt_printf.cpp
    xam_loader.cpp
    xam_user.cpp
    xam_notify.cpp
    xam_overlapped.cpp
    xnet_null.cpp
    kernel_misc.cpp
    kernel_time.cpp
    kernel_timer.cpp
    kernel_objects.cpp
    kernel_events.cpp
    kernel_thread.cpp
    kernel_object_api.cpp
    kernel_video.cpp
    pm4_trace.cpp
    vd_null_gpu.cpp
    gpu_present.cpp
    gpu_draw.cpp
    hle_d3d.cpp
    kernel_rtl.cpp
    kernel_config.cpp
    kernel_dispatcher.cpp
    kernel_spinlock.cpp
    xaudio_null.cpp
    guest_filesystem.cpp
    kernel_file.cpp
    "${IMPORT_STUBS}")

target_include_directories(gears1 PRIVATE .)
target_link_libraries(gears1 PRIVATE gears_ppc XenonUtils lucent::lucent)

# The host graphics backend. Vulkan because extern/xenia's SPIR-V shader
# translator is the intended path for real content later, and SDL3 for the
# window and its Vulkan surface.
#
# Both are optional at BUILD time as well as at run time: without them
# gpu_present.cpp compiles to the headless stub and the runtime behaves exactly
# as it did before. A machine that cannot present must still be able to build
# and run the measurement harnesses.
find_package(Vulkan QUIET)
find_package(SDL3 QUIET CONFIG)
if(Vulkan_FOUND AND SDL3_FOUND)
    target_compile_definitions(gears1 PRIVATE GEARS_HAVE_PRESENTER)
    target_link_libraries(gears1 PRIVATE Vulkan::Vulkan SDL3::SDL3)
    message(STATUS "gears1: host presenter enabled (Vulkan ${Vulkan_VERSION}, SDL3)")
else()
    message(WARNING
        "gears1: no host presenter -- Vulkan and SDL3 development packages are "
        "required for a window. The runtime will build and run headless.")
endif()

# The guest-draw graphics backend (runtime/gpu_draw.cpp): the first real draw.
# It needs Vulkan (its own headless device -- no window, so no SDL3) and Xenia's
# Xenos->SPIR-V translator (xenia_gpu). Optional like everything else: without
# either it compiles to a stub and RenderHotDraw reports it is unavailable.
if(Vulkan_FOUND AND GEARS_HAVE_XENOS_TRANSLATOR)
    # The Xenos->SPIR-V translation lives in its own static library so xenia_gpu's
    # PUBLIC compile definitions (VK_NO_PROTOTYPES, which would strip the Vulkan
    # function prototypes gpu_draw.cpp/gpu_present.cpp need, and its bundled
    # Vulkan-Headers) are consumed HERE and not re-exported to gears1. Linking
    # xenia_gpu PRIVATE means gears1 still gets its symbols at the final link but
    # none of its header world.
    add_library(gears_draw_xlate STATIC gpu_draw_xlate.cpp)
    target_compile_definitions(gears_draw_xlate PRIVATE GEARS_HAVE_GUEST_DRAW)
    target_include_directories(gears_draw_xlate PRIVATE .)
    target_link_libraries(gears_draw_xlate PRIVATE xenia_gpu lucent::lucent)
    target_compile_definitions(gears1 PRIVATE GEARS_HAVE_GUEST_DRAW)
    target_link_libraries(gears1 PRIVATE Vulkan::Vulkan gears_draw_xlate)
    message(STATUS "gears1: guest-draw backend enabled (Vulkan + Xenos translator)")
else()
    message(WARNING
        "gears1: no guest-draw backend -- needs Vulkan and extern/xenia's "
        "translator. The first-draw milestone will be unavailable.")
endif()
