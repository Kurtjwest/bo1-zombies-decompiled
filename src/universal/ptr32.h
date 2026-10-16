#pragma once
// 32-bit pointers inside the game's fixed-layout data structures.
//
// The engine's asset structures (everything reachable from XAssetHeader) are loaded straight out of the original
// Black Ops fastfiles, whose memory images use 32-bit pointers; db_load.cpp reads them with the 32-bit sizes baked
// in. A 64-bit build therefore keeps those structures byte-identical to the 32-bit build and stores their pointer
// members as Ptr32<T>: a 32-bit handle that converts to and from T* implicitly.
//
// On 32-bit Windows Ptr32<T> is plain T*. On 64-bit targets it holds a 32-bit handle (bo1ptr::Encode/Decode):
//   [0, 0x10000)                 small integers (null, sentinels, ints kept in pointers): the value itself
//   [0x10000, kWindowBase)       an offset into the 3.25 GB arena that all engine heaps live in (VirtualAlloc, hunk,
//                                PMem, malloc, thread stacks), see src/platform/win32compat/arena.cpp
//   [kWindowBase, kEscapeBase)   linear windows over the executable image (globals, string literals, functions) and
//                                the main thread's stack
//   [kEscapeBase, kNegativeBase) entries of an escape table for any other address (system heap, dylibs)
//   [kNegativeBase, 2^32)        small negative integers, among them (T*)-1 / (T*)-2, the fastfile loader's
//                                "follows inline" / "alias" markers
// Within the arena and the windows, handle arithmetic is address arithmetic, so code written for 32-bit pointers
// can keep doing pointer math on 32-bit integers (BO1_P2I / BO1_I2P in bo1_ptrcast.h convert at the casts).

#include <cstdint>
#include <cstddef>
#include <type_traits>

#if defined(_WIN32) && !defined(_WIN64)

// Ptr32<T> is T* itself: overloads written for the class form must not be declared
#define BO1_PTR32_IS_CLASS 0
template <typename T> using Ptr32 = T *;
// pointer typedefs (HANDLE, HWND, ...) as 32-bit pointer fields
template <typename P> using Ptr32T = P;

inline uint32_t Ptr32_Encode(const volatile void *p) { return (uint32_t)p; }
inline void *Ptr32_Decode(uint32_t v) { return (void *)v; }
#define BO1_NATIVE_ARRAY(a, n) (a)

#else

#define BO1_PTR32_IS_CLASS 1

namespace bo1ptr
{
constexpr uint32_t kSmallLimit = 0x10000u;     // handles below are the integer itself
constexpr uint32_t kWindowBase = 0xD0000000u;  // the arena spans handles [kSmallLimit, kWindowBase)
constexpr uint32_t kEscapeBase = 0xF0000000u;
constexpr uint32_t kNegativeBase = 0xFFFF0000u; // sign-extended small negative integers
constexpr uint32_t kMarkerAlias = 0xFFFFFFFEu;
constexpr uint32_t kMarkerInline = 0xFFFFFFFFu;
constexpr uintptr_t kArenaSize = kWindowBase;

extern char *g_arenaBase; // base of the arena reservation (its first 64 KB are never mapped)

uint32_t EncodeSlow(const volatile void *p); // anything outside the arena (or before the arena exists)
void *DecodeSlow(uint32_t v);                // any handle outside the arena range
void InitArena();                            // reserves the arena; idempotent

inline uint32_t Encode(const volatile void *p)
{
    uintptr_t d = (uintptr_t)p - (uintptr_t)g_arenaBase;
    if (__builtin_expect(g_arenaBase != nullptr && d - kSmallLimit < kWindowBase - kSmallLimit, 1))
        return (uint32_t)d;
    return EncodeSlow(p);
}

inline void *Decode(uint32_t v)
{
    if (__builtin_expect(v - kSmallLimit < kWindowBase - kSmallLimit, 1))
        return g_arenaBase + v;
    return DecodeSlow(v);
}

// Integer -> pointer for decompiled code that keeps pointers in 32-bit integers: signed values are sign-extended,
// as a 32-bit build would (so (T*)(short)-1 is still (T*)-1).
template <typename I> inline void *DecodeInt(I v)
{
    static_assert(std::is_integral_v<I> || std::is_enum_v<I>, "DecodeInt takes an integer");
    if constexpr (std::is_enum_v<I>)
        return Decode((uint32_t)(int64_t)v);
    else if constexpr (std::is_signed_v<I>)
        return Decode((uint32_t)(int32_t)v);
    else
        return Decode((uint32_t)v);
}

// Pointer (object or function) -> handle
template <typename T> inline uint32_t EncodeAny(T *p) { return Encode((const volatile void *)p); }
inline uint32_t EncodeAny(std::nullptr_t) { return 0; }

bool InArena(const volatile void *p);

// Maps [addr, addr + size) onto a range of window handles (startup: the executable image, the main thread stack).
uint32_t RegisterWindow(const void *addr, size_t size);
} // namespace bo1ptr

inline uint32_t Ptr32_Encode(const volatile void *p) { return bo1ptr::Encode(p); }
inline void *Ptr32_Decode(uint32_t v) { return bo1ptr::Decode(v); }

template <typename T> struct Ptr32
{
    uint32_t v;

    Ptr32() = default;
    Ptr32(const Ptr32 &) = default;
    Ptr32 &operator=(const Ptr32 &) = default;
    // (no separate nullptr_t overloads: "p = 0" and "p = nullptr" both go through T*, unambiguously)
    Ptr32(T *p) : v(bo1ptr::Encode((const volatile void *)p)) {}
    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>>>
    Ptr32(const Ptr32<U> &o) : v(o.v)
    {
    }

    T *get() const { return (T *)bo1ptr::Decode(v); }
    operator T *() const { return get(); }
    template <typename U, typename = std::enable_if_t<!std::is_same_v<U, T>>> explicit operator U *() const { return (U *)get(); }
    explicit operator uintptr_t() const { return (uintptr_t)get(); }
    // (int)ptr / (unsigned int)ptr: the 32-bit handle, which is what decompiled code computes with
    template <typename I, typename = std::enable_if_t<std::is_integral_v<I> && !std::is_same_v<I, bool> && sizeof(I) <= 4>>
    explicit operator I() const
    {
        return (I)v;
    }

    T *operator->() const { return get(); }
    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U> && !std::is_function_v<U>>> U &operator*() const { return *get(); }

    Ptr32 &operator=(T *p)
    {
        v = bo1ptr::Encode((const volatile void *)p);
        return *this;
    }
    // Handles encode addresses, not types: converting between Ptr32<U> and Ptr32<T> copies the handle.
    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U *, T *> && !std::is_same_v<U, T>>>
    Ptr32 &operator=(const Ptr32<U> &o)
    {
        v = o.v;
        return *this;
    }

    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U> && !std::is_function_v<U>>> Ptr32 &operator+=(ptrdiff_t n)
    {
        return *this = get() + n;
    }
    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U> && !std::is_function_v<U>>> Ptr32 &operator-=(ptrdiff_t n)
    {
        return *this = get() - n;
    }
    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U> && !std::is_function_v<U>>> Ptr32 &operator++()
    {
        return *this = get() + 1;
    }
    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U> && !std::is_function_v<U>>> Ptr32 &operator--()
    {
        return *this = get() - 1;
    }
    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U> && !std::is_function_v<U>>> T *operator++(int)
    {
        T *old = get();
        *this = old + 1;
        return old;
    }
    template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U> && !std::is_function_v<U>>> T *operator--(int)
    {
        T *old = get();
        *this = old - 1;
        return old;
    }

    // Calls through a function-pointer member (Ptr32<void(int)> cb; cb(3);) use operator T*(): a conversion to a
    // function pointer makes the object callable with the function's exact parameter types.
};

// pointer typedefs (HANDLE, HWND, ...) as 32-bit pointer fields
template <typename P> using Ptr32T = Ptr32<std::remove_pointer_t<P>>;

namespace bo1ptr
{
// a Ptr32 already holds its handle
template <typename T> inline uint32_t EncodeAny(const Ptr32<T> &p) { return p.v; }

// A native T*[] copy of an array of 32-bit pointers, for functions that take an array of native pointers
// (const char **list, int count). Lives until the end of the full expression it is created in.
template <typename T> struct NativeArray
{
    T **p;
    T *small[64];
    T **big = nullptr;
    NativeArray(const Ptr32<T> *src, size_t n)
    {
        p = n <= 64 ? small : (big = new T *[n]);
        for (size_t i = 0; i < n; ++i)
            p[i] = src[i].get();
    }
    NativeArray(const NativeArray &) = delete;
    NativeArray &operator=(const NativeArray &) = delete;
    ~NativeArray() { delete[] big; }
    operator T **() { return p; }
};
template <typename T> inline NativeArray<T> MakeNativeArray(const Ptr32<T> *src, size_t n) { return NativeArray<T>(src, n); }
template <typename T> inline NativeArray<T> MakeNativeArray(const Ptr32<Ptr32<T>> &src, size_t n) { return NativeArray<T>(src.get(), n); }
} // namespace bo1ptr
// (T **)-typed argument from an array of 32-bit pointers (a temporary native copy, valid for the call)
#define BO1_NATIVE_ARRAY(a, n) (::bo1ptr::MakeNativeArray((a), (n)))

static_assert(sizeof(Ptr32<int>) == 4, "Ptr32 must be 4 bytes");
static_assert(std::is_trivially_copyable_v<Ptr32<int>>, "Ptr32 must be trivially copyable");
static_assert(std::is_trivially_default_constructible_v<Ptr32<int>>, "Ptr32 must be usable in unions");

template <typename T> inline bool operator==(const Ptr32<T> &a, const Ptr32<T> &b) { return a.v == b.v; }
template <typename T> inline bool operator!=(const Ptr32<T> &a, const Ptr32<T> &b) { return a.v != b.v; }
template <typename T, typename U> inline bool operator==(const Ptr32<T> &a, U *b) { return a.get() == b; }
template <typename T, typename U> inline bool operator!=(const Ptr32<T> &a, U *b) { return a.get() != b; }
template <typename T, typename U> inline bool operator==(U *b, const Ptr32<T> &a) { return a.get() == b; }
template <typename T, typename U> inline bool operator!=(U *b, const Ptr32<T> &a) { return a.get() != b; }
template <typename T> inline bool operator==(const Ptr32<T> &a, std::nullptr_t) { return a.v == 0; }
template <typename T> inline bool operator!=(const Ptr32<T> &a, std::nullptr_t) { return a.v != 0; }
template <typename T> inline bool operator==(std::nullptr_t, const Ptr32<T> &a) { return a.v == 0; }
template <typename T> inline bool operator!=(std::nullptr_t, const Ptr32<T> &a) { return a.v != 0; }

#endif

// Raw 32-bit handle stored in a Ptr32 field (the fastfile loader inspects these before converting them).
template <typename P> inline uint32_t &Ptr32_Raw(P &field)
{
    static_assert(sizeof(P) == 4, "Ptr32_Raw needs a 32-bit pointer field");
    return *reinterpret_cast<uint32_t *>(&field);
}
template <typename P> inline uint32_t Ptr32_Raw(const P &field)
{
    static_assert(sizeof(P) == 4, "Ptr32_Raw needs a 32-bit pointer field");
    return *reinterpret_cast<const uint32_t *>(&field);
}
