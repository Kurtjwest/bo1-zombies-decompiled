# Cross-compiling the Linux game with clang for another processor:
#
#   cmake -S . -B build-linux-i686 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux.cmake \
#         -DBO1_LINUX_TARGET_ARCH=i686 [-DCMAKE_SYSROOT=/path/to/sysroot]
#
# (./build-linux.sh --arch <arch> does this.) BO1_LINUX_TARGET_ARCH: x86_64, i686 (32-bit x86), aarch64 (64-bit ARM) or
# armhf (32-bit ARM, ARMv7 with NEON, hard float). Clang builds for the target directly; the linker is lld.
#
# The target's C library, libstdc++ and libraries come from, in this order:
#   - a sysroot (CMAKE_SYSROOT; build-linux.sh passes BO1_SYSROOT): a copy of the target system's /usr, /lib
#   - Debian / Ubuntu multiarch: the target's packages installed next to the host's (dpkg --add-architecture i386;
#     apt install libsdl2-dev:i386 g++-multilib), in /usr/lib/<triple> and /usr/include/<triple>
#   - the cross toolchain packages (g++-aarch64-linux-gnu, g++-arm-linux-gnueabihf: /usr/<triple>)
# SDL2 is looked up with pkg-config in the same places; a target without SDL2 development files gets SDL2 built from
# source for linking (cmake/sdl2.cmake), and uses the target system's own SDL2 when the game runs.

if(NOT BO1_LINUX_TARGET_ARCH)
    set(BO1_LINUX_TARGET_ARCH "$ENV{BO1_LINUX_TARGET_ARCH}")
endif()
if(BO1_LINUX_TARGET_ARCH MATCHES "^(x86_64|amd64)$")
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
    set(_bo1_triple x86_64-linux-gnu)
    set(_bo1_multiarch x86_64-linux-gnu)
    set(_bo1_flags "")
elseif(BO1_LINUX_TARGET_ARCH MATCHES "^(i686|i386|x86)$")
    set(CMAKE_SYSTEM_PROCESSOR i686)
    set(_bo1_triple i686-linux-gnu)
    set(_bo1_multiarch i386-linux-gnu)
    set(_bo1_flags "")
elseif(BO1_LINUX_TARGET_ARCH MATCHES "^(aarch64|arm64)$")
    set(CMAKE_SYSTEM_PROCESSOR aarch64)
    set(_bo1_triple aarch64-linux-gnu)
    set(_bo1_multiarch aarch64-linux-gnu)
    set(_bo1_flags "")
elseif(BO1_LINUX_TARGET_ARCH MATCHES "^(armhf|arm|armv7)$")
    set(CMAKE_SYSTEM_PROCESSOR armv7l)
    set(_bo1_triple arm-linux-gnueabihf)
    set(_bo1_multiarch arm-linux-gnueabihf)
    # (the engine's SSE code runs on NEON through sse2neon)
    set(_bo1_flags "-march=armv7-a -mfpu=neon -mfloat-abi=hard")
else()
    message(FATAL_ERROR "BO1_LINUX_TARGET_ARCH must be x86_64, i686, aarch64 or armhf (it is '${BO1_LINUX_TARGET_ARCH}')")
endif()
# (for try_compile and the dependency builds)
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES BO1_LINUX_TARGET_ARCH)

set(CMAKE_SYSTEM_NAME Linux)
if(NOT CMAKE_C_COMPILER)
    find_program(CMAKE_C_COMPILER NAMES clang clang-20 clang-19 clang-18 clang-17 clang-16 clang-15 clang-14 REQUIRED)
endif()
if(NOT CMAKE_CXX_COMPILER)
    find_program(CMAKE_CXX_COMPILER NAMES clang++ clang++-20 clang++-19 clang++-18 clang++-17 clang++-16 clang++-15 clang++-14
                 REQUIRED)
endif()
set(CMAKE_C_COMPILER_TARGET ${_bo1_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_bo1_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_bo1_triple})

# the processor flags every target object needs (cmake/deps_target.cmake passes them to FFmpeg and libvpx too)
set(BO1_TARGET_CFLAGS "${_bo1_flags}")
set(CMAKE_C_FLAGS_INIT "${_bo1_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_bo1_flags}")

# lld links for any target (the host's GNU ld only for its own)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
find_program(CMAKE_AR NAMES llvm-ar llvm-ar-20 llvm-ar-19 llvm-ar-18 ar)
find_program(CMAKE_RANLIB NAMES llvm-ranlib llvm-ranlib-20 llvm-ranlib-19 llvm-ranlib-18 ranlib)
find_program(CMAKE_NM NAMES llvm-nm llvm-nm-20 llvm-nm-19 llvm-nm-18 nm)

# pkg-config: the target's .pc files only
if(CMAKE_SYSROOT)
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
    set(ENV{PKG_CONFIG_LIBDIR}
        "${CMAKE_SYSROOT}/usr/lib/${_bo1_multiarch}/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
    set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
else()
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
    set(ENV{PKG_CONFIG_LIBDIR}
        "/usr/lib/${_bo1_multiarch}/pkgconfig:/usr/${_bo1_triple}/lib/pkgconfig:/usr/share/pkgconfig")
    set(CMAKE_FIND_ROOT_PATH "/usr/${_bo1_triple}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
