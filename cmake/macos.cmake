# Native macOS build of the engine, for Apple silicon (arm64) or Intel (x86_64) Macs.
#
#   cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release [-DCMAKE_OSX_ARCHITECTURES=x86_64]
#   cmake --build build-mac
#
# Produces build-mac/BO1Zombies.app (a native Mach-O executable inside an app bundle). One architecture per build
# (FFmpeg and libvpx are built for it): build-mac.sh --universal builds both and joins them into one app. No Windows SDK,
# DirectX SDK or emulation layer is involved: the Win32 API surface the engine uses is implemented on
# POSIX/Mach/Cocoa (src/platform/win32compat, src/platform/macos), Direct3D 9 is implemented on Metal
# (src/platform/d3d9mtl), XAudio2 on CoreAudio and XInput on GameController. What the macOS and Linux builds share is
# in cmake/port.cmake.
#
# Cross-compiling from Linux (CI) works too: see cmake/toolchain-macos.cmake.

list(LENGTH CMAKE_OSX_ARCHITECTURES BO1_MAC_ARCH_COUNT)
if(NOT BO1_MAC_ARCH_COUNT EQUAL 1 OR NOT CMAKE_OSX_ARCHITECTURES MATCHES "^(arm64|x86_64)$")
    message(FATAL_ERROR "CMAKE_OSX_ARCHITECTURES must be one of arm64, x86_64 (it is '${CMAKE_OSX_ARCHITECTURES}'); "
                        "build-mac.sh --universal builds both and joins them")
endif()

# ---- the macOS platform layer: Cocoa windows and input, CoreAudio, GameController, ImageIO, the Metal renderer ----
file(GLOB BO1_PORT_PLATFORM_SOURCES CONFIGURE_DEPENDS
    "${SRC_DIR}/platform/macos/*.cpp"
    "${SRC_DIR}/platform/macos/*.mm"
    "${SRC_DIR}/platform/d3d9mtl/*.mm"
)
set(BO1_PORT_MOJOSHADER_PROFILE METAL)
include("${CMAKE_CURRENT_LIST_DIR}/port.cmake")

# ---- app bundle ----
set_target_properties(${BIN_NAME} PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "BO1 Zombies"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.bo1zombies.engine"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_INFO_PLIST "${PLATFORM_DIR}/macos/Info.plist.in"
    OUTPUT_NAME "BO1Zombies"
    OBJCXX_STANDARD 20
)

target_compile_definitions(${BIN_NAME} PRIVATE
    BO1_MACOS=1
    GL_SILENCE_DEPRECATION
)
target_compile_options(${BIN_NAME} PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

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

# ---- the repository's own game files (mods/, data/main/) inside the app: it installs them into its game folder,
# ~/Library/Application Support/BO1Zombies, when it starts (main_posix.cpp). Rebuilt every build: a file removed from
# the repository leaves the bundle, and the app then removes it from the game folder too. ----
add_custom_target(bundle_game_data ALL
    COMMAND ${CMAKE_COMMAND} -E ${BO1_RM_DIR} "$<TARGET_BUNDLE_CONTENT_DIR:${BIN_NAME}>/Resources/gamedata"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/mods"
        "$<TARGET_BUNDLE_CONTENT_DIR:${BIN_NAME}>/Resources/gamedata/mods"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data/main"
        "$<TARGET_BUNDLE_CONTENT_DIR:${BIN_NAME}>/Resources/gamedata/main"
    COMMENT "Bundling mods/ and data/main/ into the app")
add_dependencies(bundle_game_data ${BIN_NAME})
