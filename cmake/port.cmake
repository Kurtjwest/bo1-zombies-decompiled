# What the macOS and Linux builds share (included by cmake/macos.cmake and cmake/linux.cmake): the engine runs on the
# Win32 compatibility layer (src/platform/win32compat) there, with Direct3D 9 implemented by src/platform/d3d9mtl
# (on Metal or OpenGL), XAudio2, DirectSound and Bink in software (src/platform/common), and the system's windowing,
# audio and controller APIs behind the bridge interfaces of src/platform/common.
#
# Before including this file the platform file sets:
#   BO1_PORT_PLATFORM_SOURCES   its own platform sources
#   BO1_PORT_MOJOSHADER_PROFILE the MojoShader output profile its renderer compiles (METAL or GLSL)

set(PLATFORM_DIR "${SRC_DIR}/platform")

# ---- sources: the engine list from cmake_files.cmake, minus the Windows-only pieces, plus the platform layer ----
get_target_property(BO1_ALL_SOURCES ${BIN_NAME} SOURCES)
set(BO1_PORT_EXCLUDE
    # NVIDIA driver API (Windows only); r_hw_nvidia.cpp keeps its no-NVAPI path (src/platform/common/nvapi_stub.cpp)
    "${SRC_DIR}/nvapi/.*"
    # Tracy profiler client: only built with TRACY_ENABLE (Windows debug builds)
    "${SRC_DIR}/tracy/.*"
    # RAD's Direct3D 9 Bink blitter helper; Bink itself is provided by src/platform/common/bink_ffmpeg.cpp
    "${SRC_DIR}/binklib/dx9rad3d\\.cpp"
)
foreach(pattern IN LISTS BO1_PORT_EXCLUDE)
    list(FILTER BO1_ALL_SOURCES EXCLUDE REGEX "^${pattern}$")
endforeach()

file(GLOB BO1_PORT_SHARED_SOURCES CONFIGURE_DEPENDS
    "${PLATFORM_DIR}/win32compat/*.cpp"
    "${PLATFORM_DIR}/common/*.cpp"
    "${PLATFORM_DIR}/d3d9mtl/*.cpp"
)
set(BO1_PLATFORM_SOURCES ${BO1_PORT_SHARED_SOURCES} ${BO1_PORT_PLATFORM_SOURCES})
file(GLOB_RECURSE BO1_PLATFORM_HEADERS CONFIGURE_DEPENDS "${PLATFORM_DIR}/*.h")
list(FILTER BO1_PLATFORM_HEADERS EXCLUDE REGEX "/third_party/")
source_group(TREE "${PLATFORM_DIR}" PREFIX "platform" FILES ${BO1_PLATFORM_SOURCES} ${BO1_PLATFORM_HEADERS})

set_property(TARGET ${BIN_NAME} PROPERTY SOURCES ${BO1_ALL_SOURCES} ${BO1_PLATFORM_SOURCES} ${BO1_PLATFORM_HEADERS})

# ---- language / compiler settings ----
set_target_properties(${BIN_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    C_STANDARD 11
)

target_compile_definitions(${BIN_NAME} PRIVATE
    BO1_MP
    BO1_PORT=1
    _CONSOLE
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_NO_DEPRECATED_NO_WARNINGS
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

# The Win32 compatibility headers must win over everything else for <windows.h>, <d3d9.h>, <xmmintrin.h>, ...
target_include_directories(${BIN_NAME} BEFORE PRIVATE "${PLATFORM_DIR}/win32compat/include")
target_include_directories(${BIN_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${SRC_DIR}"
    "${SRC_DIR}/libs"
    "${SRC_DIR}/libs/libtomcrypt-1.17/src/headers"
    "${SRC_DIR}/libs/libtommath-1.0"
    "${SRC_DIR}/libs/libvpx-1.5.0/include"
    "${PLATFORM_DIR}"
)

include("${CMAKE_CURRENT_LIST_DIR}/clang_flags.cmake")
set(BO1_PORT_COMMON_FLAGS
    ${BO1_CLANG_LANGUAGE_FLAGS}
    # Every engine translation unit sees the MSVC CRT names, as MSVC's headers provide them (C/C++ only: the
    # Objective-C++ Cocoa/Metal files must not see the Windows names, Cocoa's BOOL is not Win32's BOOL).
    "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${PLATFORM_DIR}/win32compat/include/bo1_platform_prefix.h>"
    ${BO1_CLANG_COMMON_WARNING_FLAGS}
)
set(BO1_PORT_CXX_FLAGS ${BO1_CLANG_CXX_FLAGS})
target_compile_options(${BIN_NAME} PRIVATE
    ${BO1_PORT_COMMON_FLAGS}
    $<$<COMPILE_LANGUAGE:CXX>:${BO1_PORT_CXX_FLAGS}>
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types>
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<CONFIG:RelWithDebInfo>:-O2 -g>
    $<$<CONFIG:Release>:-O2>
)

# The bundled third-party libraries are portable C that never relied on MSVC's volatile semantics, and some of them
# use volatile for unaligned access: minilzo reads and writes its match data through 'volatile uint32_t *' at any
# byte offset. With -fms-volatile those accesses become load-acquire/store-release instructions, which raise an
# alignment fault (SIGBUS) on an address that is not naturally aligned, so they are compiled with plain volatile.
set(BO1_PORT_THIRD_PARTY_SOURCES ${BO1_ALL_SOURCES})
# (matched without the checkout's own path, which may contain characters that mean something in a regex)
list(FILTER BO1_PORT_THIRD_PARTY_SOURCES INCLUDE REGEX
    "/src/(minilzo|zlib|jpeg|groupvoice/speex|libs/libtomcrypt-1\\.17|libs/libtommath-1\\.0)/.*\\.(c|cpp)$")
set_source_files_properties(${BO1_PORT_THIRD_PARTY_SOURCES} PROPERTIES COMPILE_OPTIONS -fno-ms-volatile)

# The platform layer implements the path-translating CRT wrappers itself (so it calls the real functions) and is
# written against standard C++ (no Windows min/max macros).
list(FILTER BO1_PLATFORM_SOURCES INCLUDE REGEX "\\.(c|cpp)$")
set_source_files_properties(${BO1_PLATFORM_SOURCES} PROPERTIES COMPILE_DEFINITIONS BO1_COMPAT_IMPL=1)

# MojoShader: Direct3D 9 shader bytecode -> the renderer's shading language (Metal Shading Language or GLSL) for the
# Direct3D 9 implementation (src/platform/third_party/mojoshader; only that profile is built, without effect or
# preshader support)
set(MOJOSHADER_DIR "${PLATFORM_DIR}/third_party/mojoshader")
string(TOLOWER "${BO1_PORT_MOJOSHADER_PROFILE}" _bo1_profile)
add_library(bo1_mojoshader STATIC
    "${MOJOSHADER_DIR}/mojoshader.c"
    "${MOJOSHADER_DIR}/mojoshader_common.c"
    "${MOJOSHADER_DIR}/profiles/mojoshader_profile_common.c"
    "${MOJOSHADER_DIR}/profiles/mojoshader_profile_${_bo1_profile}.c"
)
target_include_directories(bo1_mojoshader PUBLIC "${MOJOSHADER_DIR}")
set(_bo1_metal 0)
set(_bo1_glsl 0)
if(BO1_PORT_MOJOSHADER_PROFILE STREQUAL "METAL")
    set(_bo1_metal 1)
else()
    set(_bo1_glsl 1)
endif()
target_compile_definitions(bo1_mojoshader PRIVATE
    SUPPORT_PROFILE_D3D=0
    SUPPORT_PROFILE_BYTECODE=0
    SUPPORT_PROFILE_HLSL=0
    SUPPORT_PROFILE_GLSL=${_bo1_glsl}
    SUPPORT_PROFILE_GLSL120=0
    SUPPORT_PROFILE_GLSLES=0
    SUPPORT_PROFILE_GLSLES3=0
    SUPPORT_PROFILE_ARB1=0
    SUPPORT_PROFILE_ARB1_NV=0
    SUPPORT_PROFILE_METAL=${_bo1_metal}
    SUPPORT_PROFILE_SPIRV=0
    SUPPORT_PROFILE_GLSPIRV=0
)
set_target_properties(bo1_mojoshader PROPERTIES C_STANDARD 99 C_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON)
target_compile_options(bo1_mojoshader PRIVATE -w $<$<CONFIG:Debug>:-O0 -g> $<$<NOT:$<CONFIG:Debug>>:-O2>)
target_link_libraries(${BIN_NAME} PRIVATE bo1_mojoshader)

# FFmpeg: xWMA sound and Bink cinematics decoders (cmake/ffmpeg.cmake)
include("${CMAKE_CURRENT_LIST_DIR}/ffmpeg.cmake")
target_link_libraries(${BIN_NAME} PRIVATE bo1_ffmpeg_avformat bo1_ffmpeg_avcodec bo1_ffmpeg_avutil)
add_dependencies(${BIN_NAME} bo1_ffmpeg)

# libvpx: the VP8 encoder of the in-game video capture (cmake/libvpx.cmake)
include("${CMAKE_CURRENT_LIST_DIR}/libvpx.cmake")
target_link_libraries(${BIN_NAME} PRIVATE bo1_libvpx_lib)
add_dependencies(${BIN_NAME} bo1_libvpx)

if(CMAKE_VERSION VERSION_LESS 3.17)
    set(BO1_RM_DIR remove_directory)
else()
    set(BO1_RM_DIR rm -rf)
endif()
