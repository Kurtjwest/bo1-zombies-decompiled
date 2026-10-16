#pragma once
// Interlocked operations and memory barriers (the winnt.h / intrin.h spellings), shared by winbase.h and intrin.h so
// the _Interlocked intrinsics resolve to these templates whichever header a translation unit includes first.
#include "wintypes.h"

// ---- interlocked operations ----
// Win32's LONG is 32 bits. The engine applies these to any 32-bit integer (LONG, int, unsigned int, DWORD), which
// MSVC accepts, and to 'long' counters, which are 64-bit on macOS: 8-byte integers get the same operation at 64 bits.
// Anything else (a pointer, a struct) is refused so a truncating use can't slip through.
#ifdef __cplusplus
#define W32_IL32(T) static_assert(sizeof(T) == 4 || (sizeof(T) == 8 && __is_integral(T)), "interlocked operation on a non-integer object")
#define W32_IL64(T) static_assert(sizeof(T) == 8, "64-bit interlocked operation on a non-64-bit object")
#define W32_ILOP(T, op32, op64) \
    if constexpr (sizeof(T) == 8) \
        return (LONG)(op64); \
    else \
        return (op32)
template <typename T> static inline LONG InterlockedIncrement(volatile T *p)
{
    W32_IL32(T);
    W32_ILOP(T, __atomic_add_fetch((volatile LONG *)p, 1, __ATOMIC_SEQ_CST), __atomic_add_fetch((volatile LONGLONG *)p, 1, __ATOMIC_SEQ_CST));
}
template <typename T> static inline LONG InterlockedDecrement(volatile T *p)
{
    W32_IL32(T);
    W32_ILOP(T, __atomic_sub_fetch((volatile LONG *)p, 1, __ATOMIC_SEQ_CST), __atomic_sub_fetch((volatile LONGLONG *)p, 1, __ATOMIC_SEQ_CST));
}
template <typename T, typename V> static inline LONG InterlockedExchange(volatile T *p, V v)
{
    W32_IL32(T);
    W32_ILOP(T, __atomic_exchange_n((volatile LONG *)p, (LONG)v, __ATOMIC_SEQ_CST), __atomic_exchange_n((volatile LONGLONG *)p, (LONGLONG)v, __ATOMIC_SEQ_CST));
}
template <typename T, typename V> static inline LONG InterlockedExchangeAdd(volatile T *p, V v)
{
    W32_IL32(T);
    W32_ILOP(T, __atomic_fetch_add((volatile LONG *)p, (LONG)v, __ATOMIC_SEQ_CST), __atomic_fetch_add((volatile LONGLONG *)p, (LONGLONG)v, __ATOMIC_SEQ_CST));
}
template <typename T, typename V> static inline LONG InterlockedAdd(volatile T *p, V v)
{
    W32_IL32(T);
    W32_ILOP(T, __atomic_add_fetch((volatile LONG *)p, (LONG)v, __ATOMIC_SEQ_CST), __atomic_add_fetch((volatile LONGLONG *)p, (LONGLONG)v, __ATOMIC_SEQ_CST));
}
template <typename T, typename V, typename C> static inline LONG InterlockedCompareExchange(volatile T *p, V exchange, C comparand)
{
    W32_IL32(T);
    if constexpr (sizeof(T) == 8)
    {
        LONGLONG c = (LONGLONG)comparand;
        __atomic_compare_exchange_n((volatile LONGLONG *)p, &c, (LONGLONG)exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return (LONG)c;
    }
    else
    {
        LONG c = (LONG)comparand;
        __atomic_compare_exchange_n((volatile LONG *)p, &c, (LONG)exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return c;
    }
}
template <typename T, typename V> static inline LONG InterlockedOr(volatile T *p, V v)
{
    W32_IL32(T);
    W32_ILOP(T, __atomic_fetch_or((volatile LONG *)p, (LONG)v, __ATOMIC_SEQ_CST), __atomic_fetch_or((volatile LONGLONG *)p, (LONGLONG)v, __ATOMIC_SEQ_CST));
}
template <typename T, typename V> static inline LONG InterlockedAnd(volatile T *p, V v)
{
    W32_IL32(T);
    W32_ILOP(T, __atomic_fetch_and((volatile LONG *)p, (LONG)v, __ATOMIC_SEQ_CST), __atomic_fetch_and((volatile LONGLONG *)p, (LONGLONG)v, __ATOMIC_SEQ_CST));
}
template <typename T, typename V> static inline LONG InterlockedXor(volatile T *p, V v)
{
    W32_IL32(T);
    W32_ILOP(T, __atomic_fetch_xor((volatile LONG *)p, (LONG)v, __ATOMIC_SEQ_CST), __atomic_fetch_xor((volatile LONGLONG *)p, (LONGLONG)v, __ATOMIC_SEQ_CST));
}
template <typename T> static inline LONGLONG InterlockedIncrement64(volatile T *p) { W32_IL64(T); return __atomic_add_fetch((volatile LONGLONG *)p, 1, __ATOMIC_SEQ_CST); }
template <typename T> static inline LONGLONG InterlockedDecrement64(volatile T *p) { W32_IL64(T); return __atomic_sub_fetch((volatile LONGLONG *)p, 1, __ATOMIC_SEQ_CST); }
template <typename T, typename V> static inline LONGLONG InterlockedExchange64(volatile T *p, V v) { W32_IL64(T); return __atomic_exchange_n((volatile LONGLONG *)p, (LONGLONG)v, __ATOMIC_SEQ_CST); }
template <typename T, typename V> static inline LONGLONG InterlockedExchangeAdd64(volatile T *p, V v) { W32_IL64(T); return __atomic_fetch_add((volatile LONGLONG *)p, (LONGLONG)v, __ATOMIC_SEQ_CST); }
template <typename T, typename V> static inline LONGLONG InterlockedAdd64(volatile T *p, V v) { W32_IL64(T); return __atomic_add_fetch((volatile LONGLONG *)p, (LONGLONG)v, __ATOMIC_SEQ_CST); }
template <typename T, typename V, typename C> static inline LONGLONG InterlockedCompareExchange64(volatile T *p, V exchange, C comparand)
{
    W32_IL64(T);
    LONGLONG c = (LONGLONG)comparand;
    __atomic_compare_exchange_n((volatile LONGLONG *)p, &c, (LONGLONG)exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}
template <typename T, typename V> static inline LONGLONG InterlockedOr64(volatile T *p, V v) { W32_IL64(T); return __atomic_fetch_or((volatile LONGLONG *)p, (LONGLONG)v, __ATOMIC_SEQ_CST); }
template <typename T, typename V> static inline LONGLONG InterlockedAnd64(volatile T *p, V v) { W32_IL64(T); return __atomic_fetch_and((volatile LONGLONG *)p, (LONGLONG)v, __ATOMIC_SEQ_CST); }
static inline PVOID InterlockedExchangePointer(PVOID volatile *p, PVOID v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline PVOID InterlockedCompareExchangePointer(PVOID volatile *p, PVOID exchange, PVOID comparand)
{
    __atomic_compare_exchange_n(p, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
// The MSVC intrinsic spellings resolve to the same templates (clang's own builtins would only take int*).
#define _InterlockedIncrement(p) InterlockedIncrement(p)
#define _InterlockedDecrement(p) InterlockedDecrement(p)
#define _InterlockedExchange(p, v) InterlockedExchange(p, v)
#define _InterlockedExchangeAdd(p, v) InterlockedExchangeAdd(p, v)
#define _InterlockedCompareExchange(p, e, c) InterlockedCompareExchange(p, e, c)
#define _InterlockedOr(p, v) InterlockedOr(p, v)
#define _InterlockedAnd(p, v) InterlockedAnd(p, v)
#define _InterlockedXor(p, v) InterlockedXor(p, v)
#define _InterlockedIncrement64(p) InterlockedIncrement64(p)
#define _InterlockedDecrement64(p) InterlockedDecrement64(p)
#define _InterlockedExchange64(p, v) InterlockedExchange64(p, v)
#define _InterlockedExchangeAdd64(p, v) InterlockedExchangeAdd64(p, v)
#define _InterlockedCompareExchange64(p, e, c) InterlockedCompareExchange64(p, e, c)
#define _InterlockedExchangePointer(p, v) InterlockedExchangePointer((PVOID volatile *)(p), (PVOID)(v))
#define _InterlockedCompareExchangePointer(p, e, c) InterlockedCompareExchangePointer((PVOID volatile *)(p), (PVOID)(e), (PVOID)(c))
#else
static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG exchange, LONG comparand)
{
    __atomic_compare_exchange_n(p, &comparand, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
#endif
#define InterlockedIncrementAcquire InterlockedIncrement
#define InterlockedDecrementRelease InterlockedDecrement
#define InterlockedCompareExchangeAcquire InterlockedCompareExchange
#define InterlockedCompareExchangeRelease InterlockedCompareExchange
static inline void MemoryBarrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#define YieldProcessor() __asm__ __volatile__("yield")
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define _ReadBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define _WriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
