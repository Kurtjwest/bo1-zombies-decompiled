// Force-included into every C/C++ translation unit of the macOS and Linux builds (CMake: -include
// bo1_platform_prefix.h). Supplies what MSVC's own headers provide implicitly: the MSVC CRT names and x86 intrinsic
// shims.
#pragma once
#if !defined(_WIN32)
// the engine runs on the Win32 compatibility layer (src/platform/win32compat): macOS, Linux
#ifndef BO1_PORT
#define BO1_PORT 1
#endif
#include "win32compat/msvc_crt.h"
#endif
