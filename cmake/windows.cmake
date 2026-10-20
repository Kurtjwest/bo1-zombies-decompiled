# The Windows game built with clang and MinGW-w64: 32-bit (x86) and 64-bit (x64) executables.
#
#   ./build-windows.sh [--arch x86_64|i686]     (any host: Linux, macOS, or Windows in an MSYS2 / Git Bash shell)
#
# or by hand with the toolchain file (cmake/toolchain-mingw.cmake). Produces <build>/BO1Zombies.exe with the
# repository's mods/ and main/ (data/main) next to it, which is also the folder the game runs in, as with the MSVC
# project: setup.ps1 copies the game's files there (or run the executable from the game's own folder).
#
# The engine runs on Windows itself here, as in the MSVC project: Win32, Direct3D 9, DirectSound, XInput and Winsock.
# What the MSVC project takes from the June 2010 DirectX SDK and its 32-bit-only libraries comes from elsewhere:
#   - XAudio2 (2.7 API): the software engine of the macOS and Linux builds (src/platform/common/xaudio2_soft.cpp, with
#     its xWMA decoding on FFmpeg), playing through WASAPI (src/platform/windows/audio_out_wasapi.cpp); no XAudio2 DLL
#   - D3DX9 and the DirectX error strings: MinGW's import libraries for d3dx9_43.dll and its dxerr9 library
#   - Bink: binkw32.dll on x86, as in the MSVC project; RAD made no 64-bit version of it, so x64 decodes the movies with
#     FFmpeg (src/platform/common/bink_ffmpeg.cpp)
#   - Steam: steam_api.dll on x86 (src/steam/redistributable_bin); x64 has steam_api64.dll built from the stand-in
#     (src/platform/common/steam_stub: no Steam), which Valve's own steam_api64.dll can replace
#   - NVAPI: the no-NVIDIA-driver answers of src/platform/common/nvapi_stub.cpp (NVIDIA's libraries are MSVC ones)
#   - libvpx (video capture): built from source, as on macOS and Linux
# A 64-bit process keeps the engine's 32-bit pointer fields as handles (src/universal/ptr32.h) over the arena that the
# macOS and Linux 64-bit builds use (src/platform/win32compat/arena.cpp, src/platform/windows/win64_memory.cpp).

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(BO1_WINDOWS_ARCH x64)
else()
    set(BO1_WINDOWS_ARCH x86)
endif()
message(STATUS "BO1 Zombies for Windows ${BO1_WINDOWS_ARCH} (clang, MinGW-w64)")

set(PLATFORM_DIR "${SRC_DIR}/platform")

# ---- sources: the engine list from cmake_files.cmake, plus the Windows platform pieces ----
get_target_property(BO1_ALL_SOURCES ${BIN_NAME} SOURCES)
set(BO1_WINDOWS_EXCLUDE
    # NVIDIA's static libraries are built for MSVC; src/platform/common/nvapi_stub.cpp answers instead
    "${SRC_DIR}/nvapi/.*"
    # the Tracy profiler client (only the MSVC Debug configuration enables it)
    "${SRC_DIR}/tracy/.*"
    # RAD's Direct3D 9 Bink blitter helper (unused by the engine)
    "${SRC_DIR}/binklib/dx9rad3d\\.cpp"
)
foreach(pattern IN LISTS BO1_WINDOWS_EXCLUDE)
    list(FILTER BO1_ALL_SOURCES EXCLUDE REGEX "^${pattern}$")
endforeach()

set(BO1_WINDOWS_PLATFORM_SOURCES
    "${PLATFORM_DIR}/common/xaudio2_soft.cpp"
    "${PLATFORM_DIR}/common/ffmpeg_media.cpp"
    "${PLATFORM_DIR}/common/nvapi_stub.cpp"
    "${PLATFORM_DIR}/windows/audio_out_wasapi.cpp"
    "${PLATFORM_DIR}/windows/windows_platform.cpp"
)
if(BO1_WINDOWS_ARCH STREQUAL "x64")
    list(APPEND BO1_WINDOWS_PLATFORM_SOURCES
        "${PLATFORM_DIR}/common/bink_ffmpeg.cpp"
        "${PLATFORM_DIR}/win32compat/arena.cpp"
        "${PLATFORM_DIR}/win32compat/arena_malloc.cpp"
        "${PLATFORM_DIR}/windows/win64_memory.cpp"
    )
endif()
file(GLOB BO1_WINDOWS_PLATFORM_HEADERS CONFIGURE_DEPENDS "${PLATFORM_DIR}/windows/include/*.h")
source_group(TREE "${PLATFORM_DIR}" PREFIX "platform" FILES ${BO1_WINDOWS_PLATFORM_SOURCES} ${BO1_WINDOWS_PLATFORM_HEADERS})
set_property(TARGET ${BIN_NAME} PROPERTY SOURCES ${BO1_ALL_SOURCES} ${BO1_WINDOWS_PLATFORM_SOURCES} ${BO1_WINDOWS_PLATFORM_HEADERS})

set_target_properties(${BIN_NAME} PROPERTIES
    OUTPUT_NAME "BO1Zombies"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    C_STANDARD 11
    WIN32_EXECUTABLE TRUE
)

target_compile_definitions(${BIN_NAME} PRIVATE
    WIN32
    _CONSOLE
    BO1_MP
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_NO_DEPRECATED_NO_WARNINGS
    BO1_WINDOWS_MINGW=1
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

# XAudio2, XAPO and DXErr headers for the software XAudio2 (src/platform/windows/include) ahead of MinGW's own
target_include_directories(${BIN_NAME} BEFORE PRIVATE "${PLATFORM_DIR}/windows/include")
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
target_compile_options(${BIN_NAME} PRIVATE
    ${BO1_CLANG_LANGUAGE_FLAGS}
    ${BO1_CLANG_COMMON_WARNING_FLAGS}
    $<$<COMPILE_LANGUAGE:CXX>:${BO1_CLANG_CXX_FLAGS}>
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types>
    # (MinGW's own headers, and the engine's 3DNow! include)
    "-Wno-#warnings"
    -Wno-pragma-pack
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<CONFIG:RelWithDebInfo>:-O2 -g>
    $<$<CONFIG:Release>:-O2>
)

# The engine's C++ sees what MSVC's headers make visible implicitly (src/platform/windows/include/bo1_mingw_prefix.h).
# Not the platform layer, which is standard C++ over <windows.h> without min/max (NOMINMAX).
set(BO1_WINDOWS_ENGINE_CXX ${BO1_ALL_SOURCES})
list(FILTER BO1_WINDOWS_ENGINE_CXX INCLUDE REGEX "\\.cpp$")
set_source_files_properties(${BO1_WINDOWS_ENGINE_CXX} PROPERTIES
    COMPILE_OPTIONS "-include;${PLATFORM_DIR}/windows/include/bo1_mingw_prefix.h")
set_source_files_properties(${BO1_WINDOWS_PLATFORM_SOURCES} PROPERTIES COMPILE_DEFINITIONS "NOMINMAX;BO1_COMPAT_IMPL=1")
# the bundled third-party C never relied on MSVC's volatile semantics (cmake/port.cmake has the details)
set(BO1_WINDOWS_THIRD_PARTY_SOURCES ${BO1_ALL_SOURCES})
list(FILTER BO1_WINDOWS_THIRD_PARTY_SOURCES INCLUDE REGEX
    "/src/(minilzo|zlib|jpeg|groupvoice/speex|libs/libtomcrypt-1\\.17|libs/libtommath-1\\.0)/.*\\.(c|cpp)$")
set_property(SOURCE ${BO1_WINDOWS_THIRD_PARTY_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -fno-ms-volatile)
if(BO1_WINDOWS_ARCH STREQUAL "x64")
    # libtomcrypt's GCC x86_64 assembly takes 64-bit words as unsigned long, which is 32 bits on Windows: its portable C
    # (the library's own sources; the engine only calls its functions)
    set(BO1_WINDOWS_TOMCRYPT_SOURCES ${BO1_ALL_SOURCES})
    list(FILTER BO1_WINDOWS_TOMCRYPT_SOURCES INCLUDE REGEX "/src/libs/libtomcrypt-1\\.17/.*\\.c$")
    set_property(SOURCE ${BO1_WINDOWS_TOMCRYPT_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS LTC_NO_ASM)
endif()

# FFmpeg: xWMA sound for the software XAudio2, and Bink 1 movies on x64 (cmake/ffmpeg.cmake)
include("${CMAKE_CURRENT_LIST_DIR}/ffmpeg.cmake")
target_link_libraries(${BIN_NAME} PRIVATE bo1_ffmpeg_avformat bo1_ffmpeg_avcodec bo1_ffmpeg_avutil)
add_dependencies(${BIN_NAME} bo1_ffmpeg)
# libvpx: the VP8 encoder of the in-game video capture (cmake/libvpx.cmake)
include("${CMAKE_CURRENT_LIST_DIR}/libvpx.cmake")
target_link_libraries(${BIN_NAME} PRIVATE bo1_libvpx_lib)
add_dependencies(${BIN_NAME} bo1_libvpx)

target_link_libraries(${BIN_NAME} PRIVATE
    d3d9
    d3dx9_43
    dsound
    dxguid
    dxerr9
    xinput1_4
    winmm
    ws2_32
    ole32
    oleaut32
    uuid
    ksguid
    dbghelp
    psapi
    shlwapi
    version
    # (FFmpeg's random seed)
    bcrypt
)
# one executable that needs nothing but Windows' own DLLs (and the game's): the C++ and MinGW runtime libraries
# linked in
target_link_options(${BIN_NAME} PRIVATE -static)

if(BO1_WINDOWS_ARCH STREQUAL "x86")
    # as the MSVC project (/arch:IA32 for this file): the GJK collision code's floats in the x87's precision
    set_property(SOURCE "${SRC_DIR}/physics/phys_gjk.cpp" APPEND PROPERTY COMPILE_OPTIONS -mno-sse -mfpmath=387)
    # as the MSVC project: a fixed image base (/DYNAMICBASE:NO, which Direct3D needs, see CMakeLists.txt) and 4 GB of
    # address space
    target_link_options(${BIN_NAME} PRIVATE -Wl,--disable-dynamicbase -Wl,--large-address-aware)
    # Bink and Steam: the 32-bit DLLs (and their import libraries) in the repository, copied next to the executable
    target_link_libraries(${BIN_NAME} PRIVATE
        "${SRC_DIR}/binklib/binkw32.lib"
        "${SRC_DIR}/steam/redistributable_bin/steam_api.lib")
    add_custom_command(TARGET ${BIN_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_DIR}/binklib/binkw32.dll"
            "${SRC_DIR}/steam/redistributable_bin/steam_api.dll" "$<TARGET_FILE_DIR:${BIN_NAME}>/"
        COMMENT "Copying binkw32.dll and steam_api.dll next to the executable")
else()
    # 64-bit: the executable's heap and VirtualAlloc reservations live in the pointer arena (arena_malloc.cpp,
    # win64_memory.cpp): its own calls (the engine, FFmpeg, libvpx, the C++ and MinGW runtime libraries) go to the
    # __wrap_ functions there, while Windows' DLLs keep their heaps
    target_link_options(${BIN_NAME} PRIVATE
        "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free"
        "-Wl,--wrap=_aligned_malloc,--wrap=_aligned_free,--wrap=_aligned_realloc,--wrap=_msize"
        "-Wl,--wrap=VirtualAlloc,--wrap=VirtualFree,--wrap=VirtualQuery")
    # steam_api64.dll: the stand-in (no Steam), next to the executable
    add_library(steam_api64 SHARED "${PLATFORM_DIR}/common/steam_stub/steam_api_stub.cpp")
    set_target_properties(steam_api64 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "steam_api64"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        CXX_STANDARD 17)
    target_link_options(steam_api64 PRIVATE -static)
    target_link_libraries(${BIN_NAME} PRIVATE steam_api64)
endif()

# ---- the repository's own game files next to the executable, as the MSVC project copies them: mods/ (loaded only
# with +set fs_game mods/<name>, see mods/sandbox/README.md) and data/main/ as main/ ----
add_custom_target(copy_mods ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/mods" "$<TARGET_FILE_DIR:${BIN_NAME}>/mods"
    COMMENT "Copying mods/ to the run folder")
add_custom_target(copy_base_data ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data/main" "$<TARGET_FILE_DIR:${BIN_NAME}>/main"
    COMMENT "Copying data/main/ to the run folder")
add_dependencies(copy_mods ${BIN_NAME})
add_dependencies(copy_base_data ${BIN_NAME})

# source paths in asserts relative to the repository, as the MSVC project's /d1trimfile
target_compile_options(${BIN_NAME} PRIVATE "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=")
