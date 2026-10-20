#pragma once
// Arena services shared by the Win32 compatibility layer and the macOS platform code (see arena.cpp).
#include <cstddef>

void *Arena_Reserve(size_t size, bool commit);         // 64 KB-granular reservation inside the arena
bool Arena_Commit(void *addr, size_t size);            // make [addr, addr+size) readable/writable
bool Arena_Decommit(void *addr, size_t size);          // drop the pages (they read back as zero after recommit)
bool Arena_Release(void *addr);                        // free a whole reservation (addr = its base)
bool Arena_QueryReservation(const void *addr, void **base, size_t *size);

void *Arena_Malloc(size_t n);  // heap inside the arena (16-byte aligned); backs the executable's malloc
void *Arena_Calloc(size_t n);
void *Arena_Realloc(void *p, size_t n);
void *Arena_Memalign(size_t alignment, size_t n); // alignment: a power of two
void Arena_Free(void *p);
size_t Arena_UsableSize(void *p);

#if !defined(__APPLE__)
// The usable size of any heap block (the arena's, or the C library's): macOS's malloc_size (arena_malloc.cpp).
extern "C" size_t malloc_size(const void *p);
#endif

void *Arena_AllocStack(size_t size, size_t *usable); // thread stack (guard page below), for CreateThread
void Arena_FreeStack(void *stack);

// Base for C++ objects whose addresses are stored in Ptr32 fields: allocates them inside the arena.
struct ArenaAllocated
{
    static void *operator new(size_t n) { return Arena_Malloc(n); }
    static void operator delete(void *p) { Arena_Free(p); }
    static void *operator new[](size_t n) { return Arena_Malloc(n); }
    static void operator delete[](void *p) { Arena_Free(p); }
};
