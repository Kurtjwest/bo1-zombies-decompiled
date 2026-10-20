# Native Linux build of the engine: x86_64, x86 (i686), arm64 (aarch64) and armhf (32-bit ARM, hard float).
#
#   cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-linux
#
# Produces build-linux/BO1Zombies (an ELF executable) with gamedata/ (the repository's mods/ and data/main/) and the
# Steam API library next to it. build-linux.sh does this for the machine's processor, or for another one with
# --arch (cross-compiling with the toolchain files in cmake/: toolchain-linux-*.cmake).
#
# The engine runs on the Win32 compatibility layer (src/platform/win32compat) as on macOS; the window, input, audio
# and controllers are SDL2's (src/platform/linux), and Direct3D 9 is implemented on OpenGL 4.5
# (src/platform/d3d9mtl over src/platform/opengl). What the macOS and Linux builds share is in cmake/port.cmake.

find_package(PkgConfig REQUIRED)

# the processor the build is for
set(BO1_LINUX_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
string(TOLOWER "${BO1_LINUX_ARCH}" BO1_LINUX_ARCH)
if(BO1_LINUX_ARCH MATCHES "^(x86_64|amd64)$")
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(BO1_LINUX_ARCH i686)
    else()
        set(BO1_LINUX_ARCH x86_64)
    endif()
elseif(BO1_LINUX_ARCH MATCHES "^(i[3-6]86|x86)$")
    set(BO1_LINUX_ARCH i686)
elseif(BO1_LINUX_ARCH MATCHES "^(aarch64|arm64)$")
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(BO1_LINUX_ARCH armhf)
    else()
        set(BO1_LINUX_ARCH aarch64)
    endif()
elseif(BO1_LINUX_ARCH MATCHES "^(arm|armv7.*|armhf)$")
    set(BO1_LINUX_ARCH armhf)
else()
    message(FATAL_ERROR "Unsupported Linux processor '${CMAKE_SYSTEM_PROCESSOR}': x86_64, i686, aarch64 and armhf are")
endif()
message(STATUS "BO1 Zombies for Linux ${BO1_LINUX_ARCH}")

# SDL2: the target's development files (pkg-config); a cross build for a target without them builds SDL2 to link
# against (cmake/sdl2.cmake)
pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
if(SDL2_FOUND)
    set(BO1_SDL2_TARGET PkgConfig::SDL2)
elseif(CMAKE_CROSSCOMPILING)
    message(STATUS "No SDL2 development files for ${BO1_LINUX_ARCH}: building SDL2 to link against")
    include("${CMAKE_CURRENT_LIST_DIR}/sdl2.cmake")
    set(BO1_SDL2_TARGET bo1_sdl2_lib)
else()
    message(FATAL_ERROR "No SDL2 development files: install them (Debian/Ubuntu: sudo apt install libsdl2-dev; Fedora: "
                        "SDL2-devel; Arch: sdl2)")
endif()

# ---- the Linux platform layer: SDL2 windows, input, audio and controllers, the OpenGL renderer ----
file(GLOB BO1_PORT_PLATFORM_SOURCES CONFIGURE_DEPENDS
    "${SRC_DIR}/platform/linux/*.cpp"
    "${SRC_DIR}/platform/opengl/*.cpp"
)
set(BO1_PORT_MOJOSHADER_PROFILE GLSL)
include("${CMAKE_CURRENT_LIST_DIR}/port.cmake")

set_target_properties(${BIN_NAME} PROPERTIES OUTPUT_NAME "BO1Zombies")

target_compile_definitions(${BIN_NAME} PRIVATE
    BO1_LINUX=1
    # the Direct3D 9 layer draws through OpenGL (GLSL shaders, src/platform/opengl/gl_rhi.cpp)
    BO1_RHI_OPENGL=1
    _GNU_SOURCE
)
# OpenGL's headers (Khronos, vendored: the renderer loads every function itself and links no GL library)
target_include_directories(${BIN_NAME} PRIVATE "${PLATFORM_DIR}/third_party/khronos")

if(BO1_LINUX_ARCH STREQUAL "armhf")
    # ARMv7 with NEON: the engine's SSE code runs on sse2neon (the compatibility layer's x86 intrinsic headers)
    target_compile_options(${BIN_NAME} PRIVATE -march=armv7-a -mfpu=neon -mfloat-abi=hard)
elseif(BO1_LINUX_ARCH STREQUAL "i686")
    # the engine's SSE intrinsics, and SSE arithmetic as the Windows game's (/arch:SSE2), not the x87's
    target_compile_options(${BIN_NAME} PRIVATE -msse2 -mfpmath=sse)
endif()

target_link_libraries(${BIN_NAME} PRIVATE ${BO1_SDL2_TARGET} pthread dl rt m)
if(TARGET bo1_sdl2)
    add_dependencies(${BIN_NAME} bo1_sdl2)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # 64-bit: the executable's C heap is the pointer arena (arena_malloc.cpp): its own calls to the malloc family go
    # to the __wrap_ functions there, while the shared libraries (the GL driver, SDL, libstdc++) keep the C library's.
    # (32-bit processes need no arena: every address is its own 32-bit handle, src/universal/ptr32.h.)
    target_link_options(${BIN_NAME} PRIVATE
        "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign,--wrap=aligned_alloc,--wrap=valloc,--wrap=memalign")
endif()
# libraries next to the executable (the Steam API)
target_link_options(${BIN_NAME} PRIVATE "-Wl,-rpath,$ORIGIN")

# ---- Steamworks: the SDK's libsteam_api.so for the processor, next to the executable. Valve ships none for 32-bit
# ARM, and the 32-bit x86 one is not in the SDK copy here: those builds use the stand-in (src/platform/common/steam_stub)
set(BO1_STEAM_SO "")
if(BO1_LINUX_ARCH STREQUAL "x86_64")
    set(BO1_STEAM_SO "${SRC_DIR}/steam/redistributable_bin/linux64/libsteam_api.so")
elseif(BO1_LINUX_ARCH STREQUAL "aarch64")
    set(BO1_STEAM_SO "${SRC_DIR}/steam/redistributable_bin/linuxarm64/libsteam_api.so")
elseif(BO1_LINUX_ARCH STREQUAL "i686" AND EXISTS "${SRC_DIR}/steam/redistributable_bin/linux32/libsteam_api.so")
    set(BO1_STEAM_SO "${SRC_DIR}/steam/redistributable_bin/linux32/libsteam_api.so")
endif()
if(BO1_STEAM_SO)
    target_link_libraries(${BIN_NAME} PRIVATE "${BO1_STEAM_SO}")
    add_custom_command(TARGET ${BIN_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BO1_STEAM_SO}" "$<TARGET_FILE_DIR:${BIN_NAME}>/"
        COMMENT "Copying libsteam_api.so next to the executable")
else()
    add_library(steam_api SHARED "${PLATFORM_DIR}/common/steam_stub/steam_api_stub.cpp")
    # (next to the executable, where its rpath finds it, as it finds Valve's)
    set_target_properties(steam_api PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        CXX_STANDARD 17)
    target_link_libraries(${BIN_NAME} PRIVATE steam_api)
endif()

# ---- the repository's own game files (mods/, data/main/) in gamedata/ next to the executable: it installs them into
# its game folder (~/.local/share/BO1Zombies) when it starts (main_posix.cpp). Rebuilt every build: a file removed from
# the repository leaves gamedata/, and the game then removes it from the game folder too. ----
add_custom_target(bundle_game_data ALL
    COMMAND ${CMAKE_COMMAND} -E ${BO1_RM_DIR} "$<TARGET_FILE_DIR:${BIN_NAME}>/gamedata"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/mods"
        "$<TARGET_FILE_DIR:${BIN_NAME}>/gamedata/mods"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data/main"
        "$<TARGET_FILE_DIR:${BIN_NAME}>/gamedata/main"
    COMMENT "Copying mods/ and data/main/ to gamedata/")
add_dependencies(bundle_game_data ${BIN_NAME})
