// The executable's C heap, inside the pointer arena.
//
// These definitions take the place of libSystem's malloc family for every call made from the executable itself
// (engine code, the statically linked FFmpeg/libvpx, the compatibility layer); system libraries keep their own heap.
// Engine allocations therefore get addresses with 32-bit handles (see arena.cpp), so decompiled code that keeps heap
// pointers in 32-bit integers works.
//
// Memory can still cross between the two heaps: a system function may hand out a block the engine later frees
// (strdup, realpath, asprintf, ...). free/realloc/malloc_size recognise such blocks (they are outside the arena) and
// pass them to the zone that owns them.

#include "arena.h"
#include <universal/ptr32.h>

#include <malloc/malloc.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <unistd.h>

namespace
{
inline bool Ours(const void *p) { return bo1ptr::InArena(p); }

void ForeignFree(void *p)
{
    malloc_zone_t *z = malloc_zone_from_ptr(p);
    if (z)
        malloc_zone_free(z, p);
}

size_t ForeignSize(const void *p)
{
    malloc_zone_t *z = malloc_zone_from_ptr(p);
    return z ? z->size(z, p) : 0;
}
} // namespace

extern "C" {

void *malloc(size_t n)
{
    void *p = Arena_Malloc(n);
    if (!p)
        errno = ENOMEM;
    return p;
}

void *calloc(size_t count, size_t size)
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

void free(void *p)
{
    if (!p)
        return;
    if (Ours(p))
        Arena_Free(p);
    else
        ForeignFree(p);
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
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

void *reallocf(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q && p)
        free(p);
    return q;
}

int posix_memalign(void **out, size_t alignment, size_t n)
{
    if (!alignment || (alignment & (alignment - 1)) || alignment % sizeof(void *))
        return EINVAL;
    void *p = Arena_Memalign(alignment, n ? n : 1);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

void *aligned_alloc(size_t alignment, size_t n)
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

void *valloc(size_t n)
{
    void *p = Arena_Memalign((size_t)getpagesize(), n ? n : 1);
    if (!p)
        errno = ENOMEM;
    return p;
}

size_t malloc_size(const void *p)
{
    if (!p)
        return 0;
    return Ours(p) ? Arena_UsableSize((void *)p) : ForeignSize(p);
}

} // extern "C"
