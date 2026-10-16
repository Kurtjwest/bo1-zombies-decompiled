// <intrin.h> for the macOS arm64 port. The MSVC intrinsics the engine uses are either clang builtins under
// -fms-extensions (_InterlockedXxx, _BitScanXxx, _rotl, __debugbreak, _byteswap_xxx, ...) or shims in
// win32compat/msvc_crt.h; SSE intrinsics come from sse2neon.
#pragma once
#include "sse_compat.h"
#include "win32compat/msvc_crt.h"
#include "win32compat/interlocked.h"
