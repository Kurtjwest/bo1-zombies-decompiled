# The Windows game built with clang and MinGW-w64 (cmake/windows.cmake), from Linux, macOS or Windows:
#
#   cmake -S . -B build-windows-x64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake \
#         -DBO1_WINDOWS_TARGET_ARCH=x86_64 -DCMAKE_BUILD_TYPE=Release
#
# (./build-windows.sh does this.) BO1_WINDOWS_TARGET_ARCH: x86_64 (64-bit Windows, the default) or i686 (32-bit).
# The compiler is the toolchain's own <triple>-clang++ when there is one (llvm-mingw, on any host; MSYS2's
# CLANG64 / CLANG32 environments), otherwise clang with --target=<triple> over the MinGW-w64 headers and libraries of
# the distribution's GCC cross toolchain (Debian/Ubuntu: sudo apt install clang lld mingw-w64). The linker is lld.

if(NOT BO1_WINDOWS_TARGET_ARCH)
    set(BO1_WINDOWS_TARGET_ARCH "$ENV{BO1_WINDOWS_TARGET_ARCH}")
endif()
if(NOT BO1_WINDOWS_TARGET_ARCH OR BO1_WINDOWS_TARGET_ARCH MATCHES "^(x86_64|amd64|x64)$")
    set(BO1_WINDOWS_TARGET_ARCH x86_64)
    set(CMAKE_SYSTEM_PROCESSOR AMD64)
elseif(BO1_WINDOWS_TARGET_ARCH MATCHES "^(i686|i386|x86|win32)$")
    set(BO1_WINDOWS_TARGET_ARCH i686)
    set(CMAKE_SYSTEM_PROCESSOR x86)
else()
    message(FATAL_ERROR "BO1_WINDOWS_TARGET_ARCH must be x86_64 or i686 (it is '${BO1_WINDOWS_TARGET_ARCH}')")
endif()
set(_bo1_triple ${BO1_WINDOWS_TARGET_ARCH}-w64-mingw32)
# (for try_compile and the dependency builds)
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES BO1_WINDOWS_TARGET_ARCH)

set(CMAKE_SYSTEM_NAME Windows)
if(NOT CMAKE_C_COMPILER)
    find_program(_bo1_cc NAMES ${_bo1_triple}-clang)
    find_program(_bo1_cxx NAMES ${_bo1_triple}-clang++)
    if(_bo1_cc AND _bo1_cxx)
        # a MinGW clang toolchain (llvm-mingw, MSYS2): its wrappers select the target and its runtime
        set(CMAKE_C_COMPILER "${_bo1_cc}")
        set(CMAKE_CXX_COMPILER "${_bo1_cxx}")
    else()
        find_program(CMAKE_C_COMPILER NAMES clang clang-20 clang-19 clang-18 clang-17 clang-16 clang-15 REQUIRED)
        find_program(CMAKE_CXX_COMPILER NAMES clang++ clang++-20 clang++-19 clang++-18 clang++-17 clang++-16 clang++-15 REQUIRED)
    endif()
endif()
set(CMAKE_C_COMPILER_TARGET ${_bo1_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_bo1_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_bo1_triple})

# 32-bit x86: SSE2 arithmetic, as the MSVC build (/arch:SSE2) and the engine's SSE code expect, not the x87's
if(BO1_WINDOWS_TARGET_ARCH STREQUAL "i686")
    set(BO1_TARGET_CFLAGS "-msse2 -mfpmath=sse")
else()
    set(BO1_TARGET_CFLAGS "")
endif()
set(CMAKE_C_FLAGS_INIT "${BO1_TARGET_CFLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${BO1_TARGET_CFLAGS}")

set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
find_program(CMAKE_AR NAMES ${_bo1_triple}-ar llvm-ar llvm-ar-20 llvm-ar-19 llvm-ar-18)
find_program(CMAKE_RANLIB NAMES ${_bo1_triple}-ranlib llvm-ranlib llvm-ranlib-20 llvm-ranlib-19 llvm-ranlib-18)
find_program(CMAKE_NM NAMES ${_bo1_triple}-nm llvm-nm llvm-nm-20 llvm-nm-19 llvm-nm-18)
find_program(CMAKE_RC_COMPILER NAMES ${_bo1_triple}-windres llvm-windres llvm-windres-20 llvm-windres-19 llvm-windres-18)

set(CMAKE_FIND_ROOT_PATH "/usr/${_bo1_triple}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
