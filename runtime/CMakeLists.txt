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
