# Cross-compile the macOS Apple silicon (arm64) build from Linux: see cmake/toolchain-macos.cmake.
set(BO1_MAC_ARCH arm64)
include("${CMAKE_CURRENT_LIST_DIR}/toolchain-macos.cmake")
