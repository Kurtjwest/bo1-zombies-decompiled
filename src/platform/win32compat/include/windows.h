// windows.h for the macOS (arm64) port: the subset of the Win32 API the engine uses, implemented on
// POSIX/Mach/Cocoa (see src/platform/win32compat and src/platform/macos).
#pragma once
#include "win32compat/wintypes.h"
#include "win32compat/winbase.h"
#include "win32compat/winuser.h"
#include "win32compat/winmisc.h"
// Like the Windows SDK's windows.h, make the sockets API available (the engine relies on this).
#ifndef WIN32_LEAN_AND_MEAN
#include "winsock2.h"
#endif

// The Windows SDK's min()/max(), which engine code relies on (it never defines NOMINMAX). As function templates rather
// than the SDK's macros: libc++'s headers #undef min/max (MSVC's STL preserves them), and templates leave member
// names and std::min/std::max alone. Mixed argument types behave like the macros (common type).
#if defined(__cplusplus) && !defined(NOMINMAX) && !defined(BO1_COMPAT_IMPL)
#include <type_traits>
template <typename A, typename B> constexpr typename std::common_type<A, B>::type max(A a, B b) { return (a > b) ? a : b; }
template <typename A, typename B> constexpr typename std::common_type<A, B>::type min(A a, B b) { return (a < b) ? a : b; }
#endif
