# libvpx (BSD), the VP8 encoder behind the in-game video capture (src/vpx/vpx.cpp, used by src/mjpeg/avi.cpp).
# The Windows build links the prebuilt src/libs/libvpx-1.5.0/libvpx.lib; the macOS port builds the same release
# from source, so the library matches the 1.5.0 headers the engine compiles against (the encoder ABI version is
# checked at vpx_codec_enc_init). Only the VP8 encoder is built.
#
# libvpx 1.5.0 predates arm64 macOS targets in its configure script, so it is built as "generic-gnu" (portable C,
# no hand-written assembly). For offline builds point BO1_LIBVPX_SOURCE_DIR at a libvpx checkout of tag v1.5.0.

include(ExternalProject)

set(BO1_LIBVPX_TAG "v1.5.0" CACHE STRING "libvpx release tag to build (must match src/libs/libvpx-1.5.0/include)")
set(BO1_LIBVPX_SOURCE_DIR "" CACHE PATH "Use this libvpx source tree instead of downloading one")

set(BO1_LIBVPX_PREFIX "${CMAKE_BINARY_DIR}/libvpx")
set(BO1_LIBVPX_INSTALL "${BO1_LIBVPX_PREFIX}/install")
set(BO1_LIBVPX_LIB "${BO1_LIBVPX_INSTALL}/lib/libvpx.a")

if(CMAKE_OSX_SYSROOT)
    set(BO1_LIBVPX_SYSROOT "${CMAKE_OSX_SYSROOT}")
elseif(MACOS_SDK)
    set(BO1_LIBVPX_SYSROOT "${MACOS_SDK}")
endif()
if(CMAKE_CROSSCOMPILING)
    # clang driving ld64.lld from a non-Apple host (cmake/toolchain-macos-arm64.cmake)
    set(BO1_LIBVPX_FLAGS "-target arm64-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET} -isysroot ${BO1_LIBVPX_SYSROOT}")
    set(BO1_LIBVPX_LDFLAGS "${BO1_LIBVPX_FLAGS} -fuse-ld=lld")
else()
    set(BO1_LIBVPX_FLAGS "-arch arm64 -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(BO1_LIBVPX_SYSROOT)
        string(APPEND BO1_LIBVPX_FLAGS " -isysroot ${BO1_LIBVPX_SYSROOT}")
    endif()
    set(BO1_LIBVPX_LDFLAGS "${BO1_LIBVPX_FLAGS}")
endif()
string(APPEND BO1_LIBVPX_FLAGS " -O2 -fPIC")

# configure reads the toolchain from the environment
set(BO1_LIBVPX_ENV
    "CC=${CMAKE_C_COMPILER}"
    "CXX=${CMAKE_CXX_COMPILER}"
    "AR=${CMAKE_AR}"
    "RANLIB=${CMAKE_RANLIB}"
    "NM=${CMAKE_NM}"
    "CFLAGS=${BO1_LIBVPX_FLAGS}"
    "CXXFLAGS=${BO1_LIBVPX_FLAGS}"
    "LDFLAGS=${BO1_LIBVPX_LDFLAGS}"
)
if(CMAKE_CROSSCOMPILING)
    # the host's GNU strip cannot process Mach-O members (it drops the archive's symbol index)
    find_program(BO1_LLVM_STRIP NAMES llvm-strip llvm-strip-18)
    if(BO1_LLVM_STRIP)
        list(APPEND BO1_LIBVPX_ENV "STRIP=${BO1_LLVM_STRIP}")
    endif()
elseif(CMAKE_STRIP)
    list(APPEND BO1_LIBVPX_ENV "STRIP=${CMAKE_STRIP}")
endif()

set(BO1_LIBVPX_CONFIGURE_ARGS
    --target=generic-gnu
    --prefix=${BO1_LIBVPX_INSTALL}
    --enable-static
    --disable-shared
    --enable-pic
    --disable-examples
    --disable-docs
    --disable-unit-tests
    --disable-install-bins
    --disable-install-docs
    --disable-vp9
    --enable-vp8
    --enable-vp8-encoder
    --disable-vp8-decoder
)

if(BO1_LIBVPX_SOURCE_DIR)
    set(BO1_LIBVPX_SOURCE_ARGS SOURCE_DIR "${BO1_LIBVPX_SOURCE_DIR}" DOWNLOAD_COMMAND "")
    set(BO1_LIBVPX_CONFIGURE "${BO1_LIBVPX_SOURCE_DIR}/configure")
else()
    set(BO1_LIBVPX_SOURCE_ARGS
        GIT_REPOSITORY "https://github.com/webmproject/libvpx.git"
        GIT_TAG "${BO1_LIBVPX_TAG}"
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        # (not <prefix>/src: ExternalProject keeps its stamp directory there, and a clone empties the source dir)
        SOURCE_DIR "${BO1_LIBVPX_PREFIX}/source"
    )
    set(BO1_LIBVPX_CONFIGURE "${BO1_LIBVPX_PREFIX}/source/configure")
endif()

find_program(BO1_MAKE_PROGRAM NAMES gmake make REQUIRED)
include(ProcessorCount)
ProcessorCount(BO1_NPROC)
if(NOT BO1_NPROC)
    set(BO1_NPROC 4)
endif()

ExternalProject_Add(bo1_libvpx
    ${BO1_LIBVPX_SOURCE_ARGS}
    PREFIX "${BO1_LIBVPX_PREFIX}"
    BINARY_DIR "${BO1_LIBVPX_PREFIX}/build"
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${BO1_LIBVPX_ENV} "${BO1_LIBVPX_CONFIGURE}" ${BO1_LIBVPX_CONFIGURE_ARGS}
    BUILD_COMMAND ${CMAKE_COMMAND} -E env ${BO1_LIBVPX_ENV} "${BO1_MAKE_PROGRAM}" -j${BO1_NPROC}
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env ${BO1_LIBVPX_ENV} "${BO1_MAKE_PROGRAM}" install
        # (an archive index for the target's linker, whatever the strip step left)
        COMMAND "${CMAKE_RANLIB}" "${BO1_LIBVPX_LIB}"
    BUILD_BYPRODUCTS "${BO1_LIBVPX_LIB}"
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
    LOG_INSTALL TRUE
    LOG_OUTPUT_ON_FAILURE TRUE
)

# The engine keeps compiling against the repository's 1.5.0 headers (src/libs/libvpx-1.5.0/include).
add_library(bo1_libvpx_lib STATIC IMPORTED GLOBAL)
set_target_properties(bo1_libvpx_lib PROPERTIES IMPORTED_LOCATION "${BO1_LIBVPX_LIB}")
add_dependencies(bo1_libvpx_lib bo1_libvpx)
