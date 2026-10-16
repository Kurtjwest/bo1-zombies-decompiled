# FFmpeg (LGPL 2.1+), built as a minimal static library set for the macOS port. Two of the game's media formats
# have no system decoder on macOS:
#   - xWMA (WMA v2) sound assets, which XAudio2 decodes on Windows (src/platform/macos/xaudio2_coreaudio.cpp)
#   - Bink 1 (.bik) cinematics, which binkw32.dll decodes on Windows (src/platform/macos/bink_mac.cpp)
# Only those decoders and the Bink demuxer are enabled; nothing else of FFmpeg is built or linked.
#
# The source is fetched from the official repository at build time. For offline builds point
# BO1_FFMPEG_SOURCE_DIR at an FFmpeg checkout (tag n6.1.2 is the tested one).

include(ExternalProject)

set(BO1_FFMPEG_TAG "n6.1.2" CACHE STRING "FFmpeg release tag to build")
set(BO1_FFMPEG_SOURCE_DIR "" CACHE PATH "Use this FFmpeg source tree instead of downloading one")

set(BO1_FFMPEG_PREFIX "${CMAKE_BINARY_DIR}/ffmpeg")
set(BO1_FFMPEG_INSTALL "${BO1_FFMPEG_PREFIX}/install")
set(BO1_FFMPEG_LIBS
    "${BO1_FFMPEG_INSTALL}/lib/libavformat.a"
    "${BO1_FFMPEG_INSTALL}/lib/libavcodec.a"
    "${BO1_FFMPEG_INSTALL}/lib/libavutil.a"
)

# compiler and flags for FFmpeg's own configure script
if(CMAKE_OSX_SYSROOT)
    set(BO1_FFMPEG_SYSROOT "${CMAKE_OSX_SYSROOT}")
elseif(MACOS_SDK)
    set(BO1_FFMPEG_SYSROOT "${MACOS_SDK}")
endif()
set(BO1_FFMPEG_ARCH_FLAGS "-arch arm64 -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
if(BO1_FFMPEG_SYSROOT)
    string(APPEND BO1_FFMPEG_ARCH_FLAGS " -isysroot ${BO1_FFMPEG_SYSROOT}")
endif()
set(BO1_FFMPEG_CC "${CMAKE_C_COMPILER}")
set(BO1_FFMPEG_EXTRA_LDFLAGS "${BO1_FFMPEG_ARCH_FLAGS}")
set(BO1_FFMPEG_CROSS_ARGS "")
if(CMAKE_CROSSCOMPILING)
    # clang driving ld64.lld from a non-Apple host (cmake/toolchain-macos-arm64.cmake)
    set(BO1_FFMPEG_ARCH_FLAGS "-target arm64-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET} -isysroot ${BO1_FFMPEG_SYSROOT}")
    set(BO1_FFMPEG_EXTRA_LDFLAGS "${BO1_FFMPEG_ARCH_FLAGS} -fuse-ld=lld")
    set(BO1_FFMPEG_CROSS_ARGS --enable-cross-compile)
endif()

set(BO1_FFMPEG_CONFIGURE_ARGS
    --prefix=${BO1_FFMPEG_INSTALL}
    --target-os=darwin
    --arch=aarch64
    ${BO1_FFMPEG_CROSS_ARGS}
    --cc=${BO1_FFMPEG_CC}
    --ar=${CMAKE_AR}
    --ranlib=${CMAKE_RANLIB}
    --nm=${CMAKE_NM}
    "--extra-cflags=${BO1_FFMPEG_ARCH_FLAGS}"
    "--extra-ldflags=${BO1_FFMPEG_EXTRA_LDFLAGS}"
    --enable-static
    --disable-shared
    --enable-pic
    --disable-everything
    --disable-autodetect
    --disable-programs
    --disable-doc
    --disable-network
    --disable-avdevice
    --disable-avfilter
    --disable-swscale
    --disable-swresample
    --disable-postproc
    --enable-decoder=wmav2,bink,binkaudio_rdft,binkaudio_dct
    --enable-demuxer=bink
)

if(BO1_FFMPEG_SOURCE_DIR)
    set(BO1_FFMPEG_SOURCE_ARGS SOURCE_DIR "${BO1_FFMPEG_SOURCE_DIR}" DOWNLOAD_COMMAND "")
    set(BO1_FFMPEG_CONFIGURE "${BO1_FFMPEG_SOURCE_DIR}/configure")
else()
    set(BO1_FFMPEG_SOURCE_ARGS
        GIT_REPOSITORY "https://github.com/FFmpeg/FFmpeg.git"
        GIT_TAG "${BO1_FFMPEG_TAG}"
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        SOURCE_DIR "${BO1_FFMPEG_PREFIX}/src"
    )
    set(BO1_FFMPEG_CONFIGURE "${BO1_FFMPEG_PREFIX}/src/configure")
endif()

find_program(BO1_MAKE_PROGRAM NAMES gmake make REQUIRED)
include(ProcessorCount)
ProcessorCount(BO1_NPROC)
if(NOT BO1_NPROC)
    set(BO1_NPROC 4)
endif()

ExternalProject_Add(bo1_ffmpeg
    ${BO1_FFMPEG_SOURCE_ARGS}
    PREFIX "${BO1_FFMPEG_PREFIX}"
    BINARY_DIR "${BO1_FFMPEG_PREFIX}/build"
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    CONFIGURE_COMMAND "${BO1_FFMPEG_CONFIGURE}" ${BO1_FFMPEG_CONFIGURE_ARGS}
    BUILD_COMMAND "${BO1_MAKE_PROGRAM}" -j${BO1_NPROC}
    INSTALL_COMMAND "${BO1_MAKE_PROGRAM}" install
    BUILD_BYPRODUCTS ${BO1_FFMPEG_LIBS}
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
    LOG_INSTALL TRUE
    LOG_OUTPUT_ON_FAILURE TRUE
)

file(MAKE_DIRECTORY "${BO1_FFMPEG_INSTALL}/include")
foreach(lib avformat avcodec avutil)
    add_library(bo1_ffmpeg_${lib} STATIC IMPORTED GLOBAL)
    set_target_properties(bo1_ffmpeg_${lib} PROPERTIES
        IMPORTED_LOCATION "${BO1_FFMPEG_INSTALL}/lib/lib${lib}.a"
        INTERFACE_INCLUDE_DIRECTORIES "${BO1_FFMPEG_INSTALL}/include")
    add_dependencies(bo1_ffmpeg_${lib} bo1_ffmpeg)
endforeach()
set_property(TARGET bo1_ffmpeg_avformat APPEND PROPERTY INTERFACE_LINK_LIBRARIES bo1_ffmpeg_avcodec)
set_property(TARGET bo1_ffmpeg_avcodec APPEND PROPERTY INTERFACE_LINK_LIBRARIES bo1_ffmpeg_avutil)
