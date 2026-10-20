// Force-included into every C/C++ translation unit of the macOS and Linux builds (CMake: -include
// bo1_platform_prefix.h). Supplies what MSVC's own headers provide implicitly: the MSVC CRT names and x86 intrinsic
// shims.
#pragma once
#if !defined(_WIN32)
// the engine runs on the Win32 compatibility layer (src/platform/win32compat): macOS, Linux
#ifndef BO1_PORT
#define BO1_PORT 1
#endif
#if defined(__linux__)
#if defined(__cplusplus)
// libstdc++'s <cstdio>, <cstdlib>, ... #undef the C library's names, which would drop msvc_crt.h's path-translating
// macros (fopen, stat, ...) in every file that includes one of them later: they are included first
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#endif
// glibc's <sys/types.h> declares a 64-bit ulong; the engine's (q_shared.h) is 32-bit, as on Windows
#include <sys/types.h>
#define ulong bo1_ulong32
#endif
#if defined(__i386__)
// 32-bit x86: the Win32 calling conventions mean nothing between functions that are all compiled from source (they
// are ignored on the other processors): the default one everywhere, as there, so declarations that differ in them
// (an engine override of an interface the compatibility layer declares) still match
#define __stdcall
#define _stdcall
#define __cdecl
#define _cdecl
#define __fastcall
#define _fastcall
#define __thiscall
#define __vectorcall
#endif
#include "win32compat/msvc_crt.h"
#endif
