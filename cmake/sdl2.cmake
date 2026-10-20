# SDL2 for a cross build whose target has no SDL2 development files (cmake/linux.cmake): SDL2 (zlib license) built
# from its official repository for the target, only to compile and link against. The game loads the target system's
# own libSDL2-2.0.so.0 (the same ABI) when it runs there; this build is not copied next to it.
#
# For offline builds point BO1_SDL2_SOURCE_DIR at an SDL checkout (tag release-2.30.9 is the tested one).

include(ExternalProject)

set(BO1_SDL2_TAG "release-2.30.9" CACHE STRING "SDL2 release tag to build for linking")
set(BO1_SDL2_SOURCE_DIR "" CACHE PATH "Use this SDL2 source tree instead of downloading one")

set(BO1_SDL2_PREFIX "${CMAKE_BINARY_DIR}/sdl2")
set(BO1_SDL2_INSTALL "${BO1_SDL2_PREFIX}/install")
set(BO1_SDL2_LIB "${BO1_SDL2_INSTALL}/lib/libSDL2-2.0.so")

if(BO1_SDL2_SOURCE_DIR)
    set(BO1_SDL2_SOURCE_ARGS SOURCE_DIR "${BO1_SDL2_SOURCE_DIR}" DOWNLOAD_COMMAND "")
else()
    set(BO1_SDL2_SOURCE_ARGS
        GIT_REPOSITORY "https://github.com/libsdl-org/SDL.git"
        GIT_TAG "${BO1_SDL2_TAG}"
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        SOURCE_DIR "${BO1_SDL2_PREFIX}/source"
    )
endif()

set(BO1_SDL2_CMAKE_ARGS
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
    -DBO1_LINUX_TARGET_ARCH=${BO1_LINUX_TARGET_ARCH}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_INSTALL_PREFIX=${BO1_SDL2_INSTALL}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_BUILD_TYPE=Release
    -DSDL_SHARED=ON
    -DSDL_STATIC=OFF
    -DSDL_TEST=OFF
    # (only the API is needed here: the system's SDL brings its video and audio drivers)
    -DSDL_X11=OFF
    -DSDL_WAYLAND=OFF
    -DSDL_KMSDRM=OFF
    -DSDL_OPENGLES=OFF
    -DSDL_VULKAN=OFF
    -DSDL_ALSA=OFF
    -DSDL_PULSEAUDIO=OFF
    -DSDL_PIPEWIRE=OFF
    -DSDL_JACK=OFF
    -DSDL_SNDIO=OFF
    -DSDL_LIBSAMPLERATE=OFF
    -DSDL_HIDAPI_LIBUSB=OFF
    -DSDL_IBUS=OFF
    -DSDL_DBUS=OFF
)
if(CMAKE_SYSROOT)
    list(APPEND BO1_SDL2_CMAKE_ARGS -DCMAKE_SYSROOT=${CMAKE_SYSROOT})
endif()

ExternalProject_Add(bo1_sdl2
    ${BO1_SDL2_SOURCE_ARGS}
    PREFIX "${BO1_SDL2_PREFIX}"
    BINARY_DIR "${BO1_SDL2_PREFIX}/build"
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    CMAKE_ARGS ${BO1_SDL2_CMAKE_ARGS}
    BUILD_BYPRODUCTS "${BO1_SDL2_LIB}"
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
    LOG_INSTALL TRUE
    LOG_OUTPUT_ON_FAILURE TRUE
)

file(MAKE_DIRECTORY "${BO1_SDL2_INSTALL}/include/SDL2")
add_library(bo1_sdl2_lib SHARED IMPORTED GLOBAL)
set_target_properties(bo1_sdl2_lib PROPERTIES
    IMPORTED_LOCATION "${BO1_SDL2_LIB}"
    IMPORTED_SONAME "libSDL2-2.0.so.0"
    INTERFACE_INCLUDE_DIRECTORIES "${BO1_SDL2_INSTALL}/include/SDL2;${BO1_SDL2_INSTALL}/include")
add_dependencies(bo1_sdl2_lib bo1_sdl2)
