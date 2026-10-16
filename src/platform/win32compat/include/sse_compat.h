// SSE/MMX intrinsics on arm64: translated to NEON by sse2neon (MIT, src/platform/third_party/sse2neon).
#pragma once
#if defined(__aarch64__) || defined(__arm64__)
// sse2neon defines __int64 as a macro unless it already is one; with -fms-extensions __int64 is a keyword, so
// shield it for the duration of the include (otherwise "unsigned __int64" would become "unsigned int64_t").
#pragma push_macro("__int64")
#ifndef __int64
#define __int64 long long
#endif
// x86 semantics for _mm_min/_mm_max (_ps/_ss): the second operand when either is NaN (or both are zeros), which
// the engine's clamps rely on to turn a NaN into the bound; NEON's fmin/fmax would propagate the NaN.
#ifndef SSE2NEON_PRECISE_MINMAX
#define SSE2NEON_PRECISE_MINMAX (1)
#endif
#include "../../third_party/sse2neon/sse2neon.h"
#pragma pop_macro("__int64")
// MMX helpers sse2neon leaves out
#ifndef BO1_MMX_EXTRA
#define BO1_MMX_EXTRA 1
static inline void _m_empty(void) {}
static inline __m64 _mm_setzero_si64(void) { return vreinterpret_m64_s64(vdup_n_s64(0)); }
static inline __m64 _mm_cvtsi32_si64(int a) { return vreinterpret_m64_s32(vset_lane_s32(a, vdup_n_s32(0), 0)); }
static inline int _mm_cvtsi64_si32(__m64 a) { return vget_lane_s32(vreinterpret_s32_m64(a), 0); }
static inline __m64 _m_punpcklwd(__m64 a, __m64 b) { return vreinterpret_m64_s16(vzip1_s16(vreinterpret_s16_m64(a), vreinterpret_s16_m64(b))); }
static inline __m64 _m_punpcklbw(__m64 a, __m64 b) { return vreinterpret_m64_s8(vzip1_s8(vreinterpret_s8_m64(a), vreinterpret_s8_m64(b))); }
static inline __m64 _m_packuswb(__m64 a, __m64 b) { return vreinterpret_m64_u8(vqmovun_s16(vcombine_s16(vreinterpret_s16_m64(a), vreinterpret_s16_m64(b)))); }
#define _mm_unpacklo_pi16 _m_punpcklwd
#define _mm_unpacklo_pi8 _m_punpcklbw
#define _mm_packs_pu16 _m_packuswb
#endif
#ifndef _MM_HINT_T0
#define _MM_HINT_T0 3
#define _MM_HINT_T1 2
#define _MM_HINT_T2 1
#define _MM_HINT_NTA 0
#endif
#else
#include_next <immintrin.h>
#endif
