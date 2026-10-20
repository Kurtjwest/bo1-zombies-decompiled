// x86 SIMD header stand-in for the ports: the compiler's own on x86 (x86_64, i386), sse2neon's NEON translation
// elsewhere (sse_compat.h).
#pragma once
#if defined(__x86_64__) || defined(__i386__)
#include_next <pmmintrin.h>
#else
#include "sse_compat.h"
#endif
