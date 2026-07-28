# The XMA decoder, built from a vendored FFmpeg fork.
#
# WHY A FORK AND NOT THE SYSTEM LIBAVCODEC. Upstream FFmpeg decodes XMA at
# PACKET level (the xma1/xma2 decoders): you hand it whole 2 KB packets and it
# hands back audio, buffering internally and withholding up to 4096 samples
# until end of stream. That is right for decoding a file and wrong for standing
# in for the console's hardware block, whose consumer paces itself per
# 128-sample subframe, polls a read offset measured in BITS, and can jump to a
# loop point mid-stream. Bridging that gap means inventing a latency and
# priming-trim compensation layer that exists in no implementation, in the one
# place where verification is hardest.
#
# The fork (Xenia's, https://github.com/has207/FFmpeg branch xmaframes) adds a
# per-FRAME codec, AV_CODEC_ID_XMAFRAMES: one reconstructed frame in, 512
# samples out, no FIFO, no trim, re-seedable at any frame boundary. That is the
# shape the hardware contract actually has, and it is the path proven across
# Xenia's compatibility library.
#
# Built here rather than committed: configure + make produce a machine-specific
# config.h, and the result is two static libraries totalling under 2 MB.
# --disable-everything with two decoders enabled keeps it that small; x86
# assembly is off because it needs nasm and buys nothing for audio decode.

include(ExternalProject)

set(FFMPEG_XMA_SOURCE_DIR "${CMAKE_SOURCE_DIR}/extern/ffmpeg-xmaframes")
set(FFMPEG_XMA_PREFIX "${CMAKE_BINARY_DIR}/ffmpeg-xma")
set(FFMPEG_XMA_INSTALL "${FFMPEG_XMA_PREFIX}/install")

if(NOT EXISTS "${FFMPEG_XMA_SOURCE_DIR}/configure")
    set(GEARS_HAVE_XMA_DECODER OFF)
    message(STATUS
        "gears1: extern/ffmpeg-xmaframes is not checked out; XMA decode is off. "
        "Run: git submodule update --init extern/ffmpeg-xmaframes")
    return()
endif()

set(FFMPEG_XMA_LIBS
    "${FFMPEG_XMA_INSTALL}/lib/libavcodec.a"
    "${FFMPEG_XMA_INSTALL}/lib/libavutil.a")

ExternalProject_Add(ffmpeg_xma_build
    SOURCE_DIR "${FFMPEG_XMA_SOURCE_DIR}"
    PREFIX "${FFMPEG_XMA_PREFIX}"
    # Out of tree, so the submodule stays clean and `git status` stays useful.
    BINARY_DIR "${FFMPEG_XMA_PREFIX}/build"
    CONFIGURE_COMMAND "${FFMPEG_XMA_SOURCE_DIR}/configure"
        --prefix=<INSTALL_DIR>
        --disable-everything
        --enable-decoder=xmaframes
        --enable-decoder=wmapro
        --enable-static --disable-shared --enable-pic
        --disable-programs --disable-doc --disable-htmlpages
        --disable-manpages --disable-podpages --disable-txtpages
        --disable-avdevice --disable-swresample --disable-swscale
        --disable-avfilter --disable-avformat
        --disable-network --disable-autodetect --disable-iconv
        --disable-debug --disable-x86asm
    INSTALL_DIR "${FFMPEG_XMA_INSTALL}"
    BUILD_COMMAND make -j
    INSTALL_COMMAND make install
    BUILD_BYPRODUCTS ${FFMPEG_XMA_LIBS}
    # The fork is pinned by the submodule, so a rebuild is only ever needed when
    # that pin moves.
    UPDATE_COMMAND ""
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON)

add_library(ffmpeg_xma INTERFACE)
add_dependencies(ffmpeg_xma ffmpeg_xma_build)
# The include directory does not exist until the external build installs, and
# CMake refuses to record a non-existent interface include dir.
file(MAKE_DIRECTORY "${FFMPEG_XMA_INSTALL}/include")
target_include_directories(ffmpeg_xma INTERFACE "${FFMPEG_XMA_INSTALL}/include")
# libavcodec depends on libavutil, so order matters for a static link.
target_link_libraries(ffmpeg_xma INTERFACE ${FFMPEG_XMA_LIBS} m)

# include() shares the caller's scope; PARENT_SCOPE here would write past the
# top-level CMakeLists and the flag would never reach the runtime target.
set(GEARS_HAVE_XMA_DECODER ON)
