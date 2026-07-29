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

# Take over the recompiler's device-store hook. ppc_context.h leaves the
# PPC_MM_STORE_* macros #ifndef-guarded precisely so a runtime can define them,
# and this must reach every translation unit BEFORE that header, so it is a
# forced include rather than an #include anyone could forget. Eleven sites in
# the whole image use it, so nothing hot is affected.
target_include_directories(gears_ppc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(gears_ppc PRIVATE -include ppc_mmio.h)

# 49k functions of straight-line translated code. The optimisation level is a
# knob because it is a real trade: -O0 keeps the build under a couple of minutes
# while the runtime is being brought up, and the translated code is exactly the
# kind of thing -- long straight-line basic blocks over a register struct --
# that an optimiser eats for breakfast. GEARS_PPC_OPT=-O1 (or -O2) rebuilds it
# optimised.
# -O2 by default. The old default was -O0 with the comment "keeps build times
# sane", and that reason turned out to be false when anyone measured it: the
# whole 49k-function image rebuilds at -O2 in 106 seconds on this machine,
# against 98 at -O1. The cost was being paid for nothing, and it was not small
# -- the title's own audio mixer took 16.7 ms per frame at -O0 and 6.7 ms at
# -O2, which is the difference between the audio pipeline missing its rate by
# 3x and missing it by 1.25x.
set(GEARS_PPC_OPT "-O2" CACHE STRING "Optimisation level for the translated guest code")
target_compile_options(gears_ppc PRIVATE -Wno-unused-variable ${GEARS_PPC_OPT})

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
    input.cpp
    hle_d3d.cpp
    kernel_rtl.cpp
    kernel_config.cpp
    kernel_dispatcher.cpp
    kernel_spinlock.cpp
    audio_out.cpp
    guest_probes.cpp
    guest_backtrace.cpp
    ppc_mmio.cpp
    wait_probe.cpp
    xaudio_null.cpp
    xma.cpp
    xma_context.cpp
    xma_decode.cpp
    guest_filesystem.cpp
    kernel_file.cpp
    "${IMPORT_STUBS}")

target_include_directories(gears1 PRIVATE .)
target_link_libraries(gears1 PRIVATE gears_ppc XenonUtils lucent::lucent)

if(GEARS_HAVE_XMA_DECODER)
    target_link_libraries(gears1 PRIVATE ffmpeg_xma)
    target_compile_definitions(gears1 PRIVATE GEARS_HAVE_XMA_DECODER=1)
endif()

# The guest-draw renderer, in its own static library rather than compiled into
# gears1 -- because it depends on NOTHING in the runtime (gpu_draw.cpp includes
# only gpu_draw.h and gpu_draw_xlate.h), which is what makes an offline frame
# replay possible. tools/frame_replay links this and renders a captured frame in
# about a second, with no guest, no menu walk and no 200-second boot; see
# frame_capture.h. gears1 links it exactly as before.
add_library(gears_draw STATIC gpu_draw.cpp frame_capture.cpp)
target_include_directories(gears_draw PUBLIC .)
target_link_libraries(gears_draw PRIVATE lucent::lucent)
target_link_libraries(gears1 PRIVATE gears_draw)

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
    # Xenia's texture_info.h GetBaseFormat is an SSSE3 shuffle on x86-64; the
    # xenia_gpu island is built with -mavx for the same reason. This TU now
    # includes it, so it needs the same target features.
    target_compile_options(gears_draw_xlate PRIVATE -mavx)
    target_compile_definitions(gears_draw PRIVATE GEARS_HAVE_GUEST_DRAW)
    target_link_libraries(gears_draw PUBLIC Vulkan::Vulkan PRIVATE gears_draw_xlate)
    target_compile_definitions(gears1 PRIVATE GEARS_HAVE_GUEST_DRAW)
    target_link_libraries(gears1 PRIVATE Vulkan::Vulkan)
    message(STATUS "gears1: guest-draw backend enabled (Vulkan + Xenos translator)")

    # Offline frame replay: render a captured frame with no guest running. This
    # is the instrument the renderer is debugged with -- one capture, then every
    # arm of a comparison renders byte-identical input in about a second.
    add_executable(frame_replay ${CMAKE_CURRENT_SOURCE_DIR}/../tools/frame_replay/main.cpp)
    target_link_libraries(frame_replay PRIVATE gears_draw lucent::lucent)
    message(STATUS "gears1: frame_replay enabled")
else()
    message(WARNING
        "gears1: no guest-draw backend -- needs Vulkan and extern/xenia's "
        "translator. The first-draw milestone will be unavailable.")
endif()
