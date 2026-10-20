# Cross-compile the macOS Intel (x86_64) build from Linux: see cmake/toolchain-macos.cmake.
set(BO1_MAC_ARCH x86_64)
include("${CMAKE_CURRENT_LIST_DIR}/toolchain-macos.cmake")
