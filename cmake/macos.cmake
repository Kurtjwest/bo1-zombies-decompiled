# Native macOS (Apple silicon, arm64) build of the engine.
#
#   cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-mac
#
# Produces build-mac/BO1Zombies.app (a native arm64 Mach-O executable inside an app bundle). No Windows SDK,
# DirectX SDK or emulation layer is involved: the Win32 API surface the engine uses is implemented on
# POSIX/Mach/Cocoa (src/platform/win32compat, src/platform/macos), Direct3D 9 is implemented on Metal
# (src/platform/d3d9mtl), XAudio2 on CoreAudio and XInput on GameController.
#
# Cross-compiling from Linux (CI) works too: see cmake/toolchain-macos-arm64.cmake.

set(PLATFORM_DIR "${SRC_DIR}/platform")

# ---- sources: the engine list from cmake_files.cmake, minus the Windows-only pieces, plus the macOS platform ----
get_target_property(BO1_ALL_SOURCES ${BIN_NAME} SOURCES)
set(BO1_MAC_EXCLUDE
    # NVIDIA driver API (Windows only); r_hw_nvidia.cpp keeps its no-NVAPI path on macOS
    "${SRC_DIR}/nvapi/.*"
    # Tracy profiler client: only built with TRACY_ENABLE (Windows debug builds)
    "${SRC_DIR}/tracy/.*"
    # RAD's Direct3D 9 Bink blitter helper; Bink itself is provided by src/platform/macos/bink_mac.cpp
    "${SRC_DIR}/binklib/dx9rad3d\\.cpp"
)
foreach(pattern IN LISTS BO1_MAC_EXCLUDE)
    list(FILTER BO1_ALL_SOURCES EXCLUDE REGEX "^${pattern}$")
endforeach()

file(GLOB BO1_PLATFORM_SOURCES CONFIGURE_DEPENDS
    "${PLATFORM_DIR}/win32compat/*.cpp"
    "${PLATFORM_DIR}/macos/*.cpp"
    "${PLATFORM_DIR}/macos/*.mm"
    "${PLATFORM_DIR}/d3d9mtl/*.cpp"
    "${PLATFORM_DIR}/d3d9mtl/*.mm"
    "${PLATFORM_DIR}/*.cpp"
)
file(GLOB_RECURSE BO1_PLATFORM_HEADERS CONFIGURE_DEPENDS "${PLATFORM_DIR}/*.h")
source_group(TREE "${PLATFORM_DIR}" PREFIX "platform" FILES ${BO1_PLATFORM_SOURCES} ${BO1_PLATFORM_HEADERS})

set_property(TARGET ${BIN_NAME} PROPERTY SOURCES ${BO1_ALL_SOURCES} ${BO1_PLATFORM_SOURCES} ${BO1_PLATFORM_HEADERS})

# ---- app bundle ----
set_target_properties(${BIN_NAME} PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "BO1 Zombies"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.bo1zombies.engine"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_INFO_PLIST "${PLATFORM_DIR}/macos/Info.plist.in"
    OUTPUT_NAME "BO1Zombies"
)

# ---- language / compiler settings ----
set_target_properties(${BIN_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    C_STANDARD 11
    OBJCXX_STANDARD 20
)

target_compile_definitions(${BIN_NAME} PRIVATE
    BO1_MP
    BO1_MACOS=1
    _CONSOLE
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_NO_DEPRECATED_NO_WARNINGS
    GL_SILENCE_DEPRECATION
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

set(BO1_MAC_COMMON_FLAGS
    # MSVC language extensions used throughout the decompiled engine (__int64, __declspec(align), __forceinline,
    # _InterlockedXxx/_BitScanXxx builtins, __debugbreak, anonymous struct members, ...)
    -fms-extensions
    # The engine (like the MSVC build) relies on wrapping signed arithmetic and on type punning.
    -fwrapv
    -fno-strict-aliasing
    # Decompiled functions sometimes fall off the end on paths that never happen; keep MSVC's behaviour
    # (an unspecified value) instead of treating those paths as unreachable.
    -fno-strict-return
    -fno-delete-null-pointer-checks
    # ... and newer Clang releases turn the warning about those paths into an error by default
    -Wno-return-type
    # Every engine translation unit sees the MSVC CRT names, as MSVC's headers provide them (C/C++ only: the
    # Objective-C++ Cocoa/Metal files must not see the Windows names, Cocoa's BOOL is not Win32's BOOL).
    "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${PLATFORM_DIR}/win32compat/include/bo1_platform_prefix.h>"
    -Wno-ignored-attributes
    -Wno-microsoft
    -Wno-deprecated-declarations
)
set(BO1_MAC_CXX_FLAGS
    # MSVC /permissive equivalents for decompiled code
    -Wno-c++11-narrowing
    -Wno-register
    -Wno-writable-strings
    -Wno-invalid-offsetof
    -Wno-unused-value
    -Wno-switch
    -Wno-parentheses
    -Wno-dangling-else
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-braces
    -Wno-null-conversion
    -Wno-tautological-compare
    -Wno-tautological-constant-out-of-range-compare
    -Wno-shift-negative-value
    -Wno-constant-conversion
    -Wno-unused-private-field
    -Wno-inconsistent-missing-override
    -Wno-delete-non-abstract-non-virtual-dtor
    -Wno-format
    -Wno-format-security
    -Wno-char-subscripts
    -Wno-unknown-pragmas
    -Wno-pointer-bool-conversion
    -Wno-undefined-var-template
    -Wno-c++20-designator
    -Wno-nonportable-include-path
    -Wno-comment
    -Werror=return-stack-address
)
target_compile_options(${BIN_NAME} PRIVATE
    ${BO1_MAC_COMMON_FLAGS}
    $<$<COMPILE_LANGUAGE:CXX>:${BO1_MAC_CXX_FLAGS}>
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types>
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<CONFIG:RelWithDebInfo>:-O2 -g>
    $<$<CONFIG:Release>:-O2>
)

# The platform layer implements the path-translating CRT wrappers itself (so it calls the real functions) and is
# written against standard C++ (no Windows min/max macros).
file(GLOB BO1_MAC_COMPAT_SOURCES "${PLATFORM_DIR}/win32compat/*.cpp" "${PLATFORM_DIR}/macos/*.cpp" "${PLATFORM_DIR}/d3d9mtl/*.cpp")
set_source_files_properties(${BO1_MAC_COMPAT_SOURCES} PROPERTIES COMPILE_DEFINITIONS BO1_COMPAT_IMPL=1)

# ---- libraries ----
find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
find_library(GAMECONTROLLER_FRAMEWORK GameController REQUIRED)
find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
find_library(CARBON_FRAMEWORK Carbon REQUIRED)
find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
find_library(APPLICATIONSERVICES_FRAMEWORK ApplicationServices REQUIRED)
find_library(COREHAPTICS_FRAMEWORK CoreHaptics REQUIRED)
find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)
find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
target_link_libraries(${BIN_NAME} PRIVATE
    ${COCOA_FRAMEWORK}
    ${METAL_FRAMEWORK}
    ${QUARTZCORE_FRAMEWORK}
    ${AUDIOTOOLBOX_FRAMEWORK}
    ${COREAUDIO_FRAMEWORK}
    ${GAMECONTROLLER_FRAMEWORK}
    ${IOKIT_FRAMEWORK}
    ${CARBON_FRAMEWORK}
    ${COREVIDEO_FRAMEWORK}
    ${APPLICATIONSERVICES_FRAMEWORK}
    ${COREHAPTICS_FRAMEWORK}
    ${IMAGEIO_FRAMEWORK}
    ${COREGRAPHICS_FRAMEWORK}
    pthread
)

# MojoShader: Direct3D 9 shader bytecode -> Metal Shading Language for the Direct3D 9 implementation
# (src/platform/third_party/mojoshader; only the Metal profile is built, without effect/preshader support)
set(MOJOSHADER_DIR "${PLATFORM_DIR}/third_party/mojoshader")
add_library(bo1_mojoshader STATIC
    "${MOJOSHADER_DIR}/mojoshader.c"
    "${MOJOSHADER_DIR}/mojoshader_common.c"
    "${MOJOSHADER_DIR}/profiles/mojoshader_profile_common.c"
    "${MOJOSHADER_DIR}/profiles/mojoshader_profile_metal.c"
)
target_include_directories(bo1_mojoshader PUBLIC "${MOJOSHADER_DIR}")
target_compile_definitions(bo1_mojoshader PRIVATE
    SUPPORT_PROFILE_D3D=0
    SUPPORT_PROFILE_BYTECODE=0
    SUPPORT_PROFILE_HLSL=0
    SUPPORT_PROFILE_GLSL=0
    SUPPORT_PROFILE_GLSL120=0
    SUPPORT_PROFILE_GLSLES=0
    SUPPORT_PROFILE_GLSLES3=0
    SUPPORT_PROFILE_ARB1=0
    SUPPORT_PROFILE_ARB1_NV=0
    SUPPORT_PROFILE_METAL=1
    SUPPORT_PROFILE_SPIRV=0
    SUPPORT_PROFILE_GLSPIRV=0
)
set_target_properties(bo1_mojoshader PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
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

# Steamworks: the SDK's universal (x86_64 + arm64) libsteam_api.dylib. Its install name is
# @loader_path/libsteam_api.dylib, so it goes next to the executable (Contents/MacOS) inside the bundle.
set(BO1_STEAM_DYLIB "${SRC_DIR}/steam/redistributable_bin/osx/libsteam_api.dylib")
target_link_libraries(${BIN_NAME} PRIVATE "${BO1_STEAM_DYLIB}")
add_custom_command(TARGET ${BIN_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BO1_STEAM_DYLIB}" "$<TARGET_FILE_DIR:${BIN_NAME}>/"
    COMMENT "Bundling libsteam_api.dylib")

target_link_options(${BIN_NAME} PRIVATE
    # Large stack for the main thread: the engine keeps big structures on the stack, as on Windows (1 MB reserve
    # there, but several worker paths assume more is available).
    "-Wl,-stack_size,0x2000000"
)

# ---- run folder: mods/ and data/main/ next to the game data, as the Windows build does ----
add_custom_target(copy_mods ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/mods" "${CMAKE_BINARY_DIR}/mods"
    COMMENT "Copying mods/ to the run folder")
add_custom_target(copy_base_data ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data/main" "${CMAKE_BINARY_DIR}/main"
    COMMENT "Copying data/main/ to the run folder")
