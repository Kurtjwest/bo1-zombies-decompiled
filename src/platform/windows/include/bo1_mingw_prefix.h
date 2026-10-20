// Force-included into every C++ translation unit of the engine in the Windows builds made with clang and MinGW
// (cmake/windows.cmake): what MSVC's own headers make visible without being asked for (the C library's headers that
// MSVC's include one another, <windows.h>'s min/max, the CRT's typedefs), so the engine compiles as it does with the
// MSVC project.
#pragma once

// winsock2.h before windows.h, which would bring in the old winsock.h otherwise
#include <winsock2.h>
#include <windows.h>

#include <ctype.h>
#include <float.h>
#include <malloc.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <intrin.h>

// <windows.h>'s min()/max(), which engine code relies on: libstdc++ #undefs the macros (and defines NOMINMAX), so they
// are function templates here, as in the macOS and Linux builds (src/platform/win32compat/include/windows.h); mixed
// argument types behave like the macros (common type), and member names and std::min/std::max are left alone.
#include <type_traits>
template <typename A, typename B> constexpr typename std::common_type<A, B>::type max(A a, B b) { return (a > b) ? a : b; }
template <typename A, typename B> constexpr typename std::common_type<A, B>::type min(A a, B b) { return (a < b) ? a : b; }

// <corecrt_search.h>'s comparison function type (qsort, bsearch)
typedef int(__cdecl *_CoreCrtNonSecureSearchSortCompareFunction)(const void *, const void *);

#if defined(_WIN64)
// __readfsdword(0x24) is the x86 TEB's current thread id, the one thing the engine reads with it
// (src/universal/profile.h); the x64 TEB is not at FS
#define __readfsdword(offset) ((unsigned long)GetCurrentThreadId())
#endif

// MSVC's _setjmp(buf) is the one-argument intrinsic. MinGW declares the C runtime's with a second argument, the frame
// a matching longjmp unwinds to: none here, so the engine's longjmps (script and fatal errors) only restore the
// registers (no unwinding through the frames in between), as on macOS and Linux. setjmp(buf) the same.
#undef setjmp
#if defined(_UCRT)
#undef _setjmp
#if defined(_WIN64)
#define _setjmp(buf) __intrinsic_setjmpex((buf), NULL)
#else
#define _setjmp(buf) __intrinsic_setjmp((buf), NULL)
#endif
#else
#define _setjmp(buf) _setjmp((buf), NULL)
#endif
#define setjmp(buf) _setjmp(buf)
