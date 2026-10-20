# Cross-compile a macOS build from Linux with LLVM (clang + ld64.lld) and a macOS SDK. BO1_MAC_ARCH picks the
# processor (arm64: Apple silicon, the default; x86_64: Intel); toolchain-macos-arm64.cmake and
# toolchain-macos-x86_64.cmake set it.
#
#   cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-macos-x86_64.cmake -DMACOS_SDK=/path/to/MacOSX14.5.sdk
#
# On a Mac none of this is needed: ./build-mac.sh (or `cmake -S . -B build-mac -DCMAKE_OSX_ARCHITECTURES=x86_64`)
# uses Xcode's toolchain.

if(NOT BO1_MAC_ARCH)
    set(BO1_MAC_ARCH arm64)
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES BO1_MAC_ARCH)
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR ${BO1_MAC_ARCH})
set(CMAKE_OSX_ARCHITECTURES ${BO1_MAC_ARCH} CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "" FORCE)

if(NOT MACOS_SDK)
    set(MACOS_SDK "$ENV{MACOS_SDK}")
endif()
# try_compile projects re-read this file; pass the SDK through to them as well.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MACOS_SDK)
set(ENV{MACOS_SDK} "${MACOS_SDK}")
if(NOT MACOS_SDK)
    message(FATAL_ERROR "Set MACOS_SDK to a MacOSX*.sdk directory")
endif()
set(CMAKE_OSX_SYSROOT "${MACOS_SDK}" CACHE PATH "" FORCE)

find_program(BO1_CLANG NAMES clang clang-18 clang-17 REQUIRED)
find_program(BO1_CLANGXX NAMES clang++ clang++-18 clang++-17 REQUIRED)
set(CMAKE_C_COMPILER "${BO1_CLANG}")
set(CMAKE_CXX_COMPILER "${BO1_CLANGXX}")
set(CMAKE_OBJC_COMPILER "${BO1_CLANG}")
set(CMAKE_OBJCXX_COMPILER "${BO1_CLANGXX}")
set(BO1_TARGET_TRIPLE "${BO1_MAC_ARCH}-apple-macos13.0")
set(CMAKE_C_COMPILER_TARGET ${BO1_TARGET_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${BO1_TARGET_TRIPLE})
set(CMAKE_OBJC_COMPILER_TARGET ${BO1_TARGET_TRIPLE})
set(CMAKE_OBJCXX_COMPILER_TARGET ${BO1_TARGET_TRIPLE})

find_program(BO1_LLVM_AR NAMES llvm-ar llvm-ar-18 REQUIRED)
find_program(BO1_LLVM_RANLIB NAMES llvm-ranlib llvm-ranlib-18 REQUIRED)
find_program(BO1_LLVM_INSTALL_NAME_TOOL NAMES llvm-install-name-tool llvm-install-name-tool-18)
set(CMAKE_AR "${BO1_LLVM_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${BO1_LLVM_RANLIB}" CACHE FILEPATH "" FORCE)
if(BO1_LLVM_INSTALL_NAME_TOOL)
    set(CMAKE_INSTALL_NAME_TOOL "${BO1_LLVM_INSTALL_NAME_TOOL}" CACHE FILEPATH "" FORCE)
endif()
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")

set(CMAKE_FIND_ROOT_PATH "${MACOS_SDK}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_FRAMEWORK FIRST)
