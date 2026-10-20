// The executable's C heap, inside the pointer arena.
//
// These definitions take the place of the C library's malloc family for every call made from the executable itself
// (engine code, the statically linked FFmpeg/libvpx, the compatibility layer); system libraries keep their own heap.
// Engine allocations therefore get addresses with 32-bit handles (see arena.cpp), so decompiled code that keeps heap
// pointers in 32-bit integers works.
//   - macOS: the executable's own definitions; the two-level namespace binds them to its own calls only.
//   - Linux: ELF would bind definitions named malloc for every library in the process (the GL driver too), so they
//     are named __wrap_malloc, ... and the link (cmake/linux.cmake) passes -Wl,--wrap=malloc,... which binds the
//     executable's own calls to them and leaves the shared libraries on the C library's heap.
//   - 64-bit Windows: the same --wrap link (cmake/windows.cmake), which covers the statically linked C++ and MinGW
//     runtime libraries too; the C runtime DLL and the system's DLLs keep their own heaps.
//
// Memory can still cross between the two heaps: a system function may hand out a block the engine later frees
// (strdup, realpath, asprintf, ...). free/realloc/malloc_size recognise such blocks (they are outside the arena) and
// pass them to the heap that owns them.

#include "arena.h"
#include <universal/ptr32.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif
#include <cerrno>
#include <cstdint>
#include <cstring>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#if BO1_PTR32_IDENTITY
// 32-bit processes: the C library's blocks are their own 32-bit handles (ptr32.h), so its heap stays the heap (the
// link wraps nothing, cmake/linux.cmake); only the usable-size query the layer uses is provided
extern "C" size_t malloc_size(const void *p) { return p ? malloc_usable_size((void *)p) : 0; }
#else
#if defined(__APPLE__)
#define BO1_HEAP(name) name
#elif defined(_WIN32)
#define BO1_HEAP(name) __wrap_##name
#else
#define BO1_HEAP(name) __wrap_##name
extern "C" void __real_free(void *p);
#endif

namespace
{
inline bool Ours(const void *p) { return bo1ptr::InArena(p); }

#if defined(_WIN32)
// The C runtime DLL's own free and _msize, for its blocks (strdup, ...). Looked up by name: the --wrap link points
// the executable's own references to them, the import thunks included, at the functions here.
FARPROC CrtFunction(const char *name)
{
#if defined(_UCRT)
    HMODULE crt = GetModuleHandleW(L"ucrtbase.dll");
#else
    HMODULE crt = GetModuleHandleW(L"msvcrt.dll");
#endif
    return crt ? GetProcAddress(crt, name) : nullptr;
}
void(__cdecl *volatile g_crtFree)(void *);
size_t(__cdecl *volatile g_crtMsize)(void *);
#endif

void ForeignFree(void *p)
{
#if defined(_WIN32)
    if (!g_crtFree)
        g_crtFree = (void(__cdecl *)(void *))(void *)CrtFunction("free");
    if (g_crtFree)
        g_crtFree(p);
#elif defined(__APPLE__)
    malloc_zone_t *z = malloc_zone_from_ptr(p);
    if (z)
        malloc_zone_free(z, p);
#else
    __real_free(p);
#endif
}

size_t ForeignSize(const void *p)
{
#if defined(_WIN32)
    if (!g_crtMsize)
        g_crtMsize = (size_t(__cdecl *)(void *))(void *)CrtFunction("_msize");
    return g_crtMsize ? g_crtMsize((void *)p) : 0;
#elif defined(__APPLE__)
    malloc_zone_t *z = malloc_zone_from_ptr(p);
    return z ? z->size(z, p) : 0;
#else
    return malloc_usable_size((void *)p);
#endif
}
} // namespace

extern "C" {

void *BO1_HEAP(malloc)(size_t n)
{
    void *p = Arena_Malloc(n);
    if (!p)
        errno = ENOMEM;
    return p;
}

void *BO1_HEAP(calloc)(size_t count, size_t size)
{
    size_t n;
    if (__builtin_mul_overflow(count, size, &n))
    {
        errno = ENOMEM;
        return nullptr;
    }
    void *p = Arena_Calloc(n);
    if (!p)
        errno = ENOMEM;
    return p;
}

void BO1_HEAP(free)(void *p)
{
    if (!p)
        return;
    if (Ours(p))
        Arena_Free(p);
    else
        ForeignFree(p);
}

void *BO1_HEAP(realloc)(void *p, size_t n)
{
    if (!p)
        return BO1_HEAP(malloc)(n);
    if (Ours(p))
    {
        void *q = Arena_Realloc(p, n ? n : 1);
        if (!q)
            errno = ENOMEM;
        return q;
    }
    // a block from a system heap moves into the arena
    const size_t have = ForeignSize(p);
    void *q = Arena_Malloc(n ? n : 1);
    if (!q)
    {
        errno = ENOMEM;
        return nullptr;
    }
    memcpy(q, p, have < n ? have : n);
    ForeignFree(p);
    return q;
}

#if defined(__APPLE__)
void *reallocf(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q && p)
        free(p);
    return q;
}
#endif

#if defined(_WIN32)
// the Microsoft C runtime's aligned blocks and block size
void *__wrap__aligned_malloc(size_t n, size_t alignment)
{
    if (!alignment || (alignment & (alignment - 1)))
    {
        errno = EINVAL;
        return nullptr;
    }
    void *p = Arena_Memalign(alignment, n ? n : 1);
    if (!p)
        errno = ENOMEM;
    return p;
}

void __wrap__aligned_free(void *p) { __wrap_free(p); }

void *__wrap__aligned_realloc(void *p, size_t n, size_t alignment)
{
    if (!p)
        return __wrap__aligned_malloc(n, alignment);
    if (!n)
    {
        __wrap_free(p);
        return nullptr;
    }
    void *q = __wrap__aligned_malloc(n, alignment);
    if (!q)
        return nullptr;
    const size_t have = Ours(p) ? Arena_UsableSize(p) : ForeignSize(p);
    memcpy(q, p, have < n ? have : n);
    __wrap_free(p);
    return q;
}

size_t __wrap__msize(void *p) { return p ? (Ours(p) ? Arena_UsableSize(p) : ForeignSize(p)) : 0; }
#else
int BO1_HEAP(posix_memalign)(void **out, size_t alignment, size_t n)
{
    if (!alignment || (alignment & (alignment - 1)) || alignment % sizeof(void *))
        return EINVAL;
    void *p = Arena_Memalign(alignment, n ? n : 1);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

void *BO1_HEAP(aligned_alloc)(size_t alignment, size_t n)
{
    if (!alignment || (alignment & (alignment - 1)))
    {
        errno = EINVAL;
        return nullptr;
    }
    void *p = Arena_Memalign(alignment, n ? n : 1);
    if (!p)
        errno = ENOMEM;
    return p;
}

void *BO1_HEAP(valloc)(size_t n)
{
    void *p = Arena_Memalign((size_t)getpagesize(), n ? n : 1);
    if (!p)
        errno = ENOMEM;
    return p;
}

#endif // !_WIN32

#if !defined(__APPLE__) && !defined(_WIN32)
void *__wrap_memalign(size_t alignment, size_t n)
{
    if (!alignment || (alignment & (alignment - 1)))
    {
        errno = EINVAL;
        return nullptr;
    }
    void *p = Arena_Memalign(alignment < sizeof(void *) ? sizeof(void *) : alignment, n ? n : 1);
    if (!p)
        errno = ENOMEM;
    return p;
}
#endif

size_t malloc_size(const void *p)
{
    if (!p)
        return 0;
    return Ours(p) ? Arena_UsableSize((void *)p) : ForeignSize(p);
}

} // extern "C"
#endif // BO1_PTR32_IDENTITY
