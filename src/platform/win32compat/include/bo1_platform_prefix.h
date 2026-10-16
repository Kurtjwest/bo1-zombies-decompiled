// Force-included into every translation unit of the macOS build (CMake: -include bo1_platform_prefix.h).
// Supplies what MSVC's own headers provide implicitly: the MSVC CRT names and x86 intrinsic shims.
#pragma once
#if defined(__APPLE__)
#include "win32compat/msvc_crt.h"
#endif
