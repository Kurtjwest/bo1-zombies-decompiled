// The 32-bit handle space behind Ptr32 (src/universal/ptr32.h) and the memory it covers.
//
// The engine was written for (and decompiled from) a 32-bit program: asset structures keep 32-bit pointer fields,
// and plenty of code keeps addresses in 32-bit integers and does arithmetic on them. A 64-bit macOS process cannot
// map anything below 4 GB, so instead every address the engine produces is given a 32-bit handle such that handle
// arithmetic is address arithmetic:
//   - the arena: one contiguous 3.25 GB reservation. VirtualAlloc (hunk, fastfile blocks, PMem), the C heap of the
//     executable (arena_malloc.cpp) and the stacks of CreateThread threads are carved out of it; a handle is the
//     offset from its base.
//   - windows: the executable image (globals, string literals, code) and the main thread's stack, mapped linearly
//     onto a handle range of their own.
//   - an escape table for any other address (system heaps, other images): such handles round-trip exactly.
// See the handle layout in ptr32.h.

#include <universal/ptr32.h>
#include "arena.h"

#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

extern "C" const struct mach_header_64 _mh_execute_header;

namespace bo1ptr
{
char *g_arenaBase;
uintptr_t g_imageWindowStart;
uint32_t g_imageWindowSize;
} // namespace bo1ptr

namespace
{
constexpr uintptr_t kGranule = 0x10000; // Windows allocation granularity; also the handle alignment of windows
constexpr uint32_t kEscapeCapacity = bo1ptr::kNegativeBase - bo1ptr::kEscapeBase;
constexpr uint32_t kEscapeHashBits = 24; // 16M slots (pages are committed on first touch)

std::atomic<int> g_arenaInitState{0}; // 0 = no, 1 = in progress, 2 = done
pthread_mutex_t g_regionLock = PTHREAD_MUTEX_INITIALIZER;

// free ranges [start, end) as offsets from the arena base, keyed by start
std::map<uintptr_t, uintptr_t> *g_free;
// live reservations: start -> size
std::map<uintptr_t, uintptr_t> *g_reserved;
size_t g_pageSize = 16384;

// windows: [addr, addr + size) <-> [handle, handle + size)
struct Window
{
    uintptr_t addr;
    uint32_t handle;
    uint32_t size;
};
constexpr int kMaxWindows = 16;
Window g_windows[kMaxWindows];
std::atomic<int> g_windowCount{0};
uint32_t g_nextWindowHandle = bo1ptr::kWindowBase;
pthread_mutex_t g_windowLock = PTHREAD_MUTEX_INITIALIZER;

// escape table
pthread_mutex_t g_escapeLock = PTHREAD_MUTEX_INITIALIZER;
std::atomic<uint32_t> g_escapeCount{0};
void *volatile *g_escapeTable; // escape index -> address
uintptr_t *g_escapeHashKeys;   // address, 0 = empty
uint32_t *g_escapeHashValues;  // escape index

void *ReserveVirtual(size_t size)
{
    void *p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

void *CommitAnywhere(size_t size)
{
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

uintptr_t AlignUp(uintptr_t v, uintptr_t a) { return (v + a - 1) & ~(a - 1); }
uintptr_t AlignDown(uintptr_t v, uintptr_t a) { return v & ~(a - 1); }

void InitEscapeTable()
{
    g_escapeHashKeys = (uintptr_t *)CommitAnywhere(sizeof(uintptr_t) << kEscapeHashBits);
    g_escapeHashValues = (uint32_t *)CommitAnywhere(sizeof(uint32_t) << kEscapeHashBits);
    g_escapeTable = (void *volatile *)CommitAnywhere(sizeof(void *) * ((size_t)1 << kEscapeHashBits) / 2);
    if (!g_escapeHashKeys || !g_escapeHashValues || !g_escapeTable)
    {
        fprintf(stderr, "[arena] cannot allocate the escape table\n");
        abort();
    }
}

// The main executable's mapped segments (__TEXT .. __DATA), without __PAGEZERO and __LINKEDIT.
void RegisterImageWindow()
{
    const mach_header_64 *mh = &_mh_execute_header;
    uintptr_t textVmaddr = 0;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    const load_command *lc = (const load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; ++i, lc = (const load_command *)((const char *)lc + lc->cmdsize))
    {
        if (lc->cmd != LC_SEGMENT_64)
            continue;
        const segment_command_64 *seg = (const segment_command_64 *)lc;
        if (!strcmp(seg->segname, "__TEXT"))
            textVmaddr = seg->vmaddr;
    }
    const intptr_t slide = (intptr_t)mh - (intptr_t)textVmaddr;
    lc = (const load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; ++i, lc = (const load_command *)((const char *)lc + lc->cmdsize))
    {
        if (lc->cmd != LC_SEGMENT_64)
            continue;
        const segment_command_64 *seg = (const segment_command_64 *)lc;
        if (!strcmp(seg->segname, "__PAGEZERO") || !strcmp(seg->segname, "__LINKEDIT") || !seg->vmsize)
            continue;
        lo = lo < seg->vmaddr + slide ? lo : seg->vmaddr + slide;
        hi = hi > seg->vmaddr + seg->vmsize + slide ? hi : seg->vmaddr + seg->vmsize + slide;
    }
    if (lo < hi && !bo1ptr::RegisterWindow((const void *)lo, hi - lo))
        fprintf(stderr, "[arena] cannot map the executable image (%zu bytes) to handles\n", (size_t)(hi - lo));
}
} // namespace

namespace bo1ptr
{
void InitArena()
{
    int expected = 0;
    if (g_arenaInitState.load(std::memory_order_acquire) == 2)
        return;
    if (!g_arenaInitState.compare_exchange_strong(expected, 1))
    {
        while (g_arenaInitState.load(std::memory_order_acquire) != 2)
            sched_yield();
        return;
    }
    g_pageSize = (size_t)sysconf(_SC_PAGESIZE);
    // Reserve the arena plus one granule of slack so the base can be aligned.
    const size_t want = (size_t)kArenaSize + kGranule;
    char *raw = (char *)ReserveVirtual(want);
    if (!raw)
    {
        fprintf(stderr, "[arena] cannot reserve %zu bytes of address space\n", want);
        abort();
    }
    char *base = (char *)AlignUp((uintptr_t)raw, kGranule);
    // the maps' nodes come from the system heap (the C++ allocator), never from the arena's own malloc
    g_free = new std::map<uintptr_t, uintptr_t>();
    g_reserved = new std::map<uintptr_t, uintptr_t>();
    // Handles below kSmallLimit are plain integers: the first granule is never handed out.
    (*g_free)[kSmallLimit] = (uintptr_t)kArenaSize;
    InitEscapeTable();
    RegisterImageWindow();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    g_arenaBase = base;
    g_arenaInitState.store(2, std::memory_order_release);
}

bool InArena(const volatile void *p)
{
    uintptr_t d = (uintptr_t)p - (uintptr_t)g_arenaBase;
    return g_arenaBase && d - kSmallLimit < kArenaSize - kSmallLimit;
}

uint32_t RegisterWindow(const void *addr, size_t size)
{
    const uintptr_t start = AlignDown((uintptr_t)addr, kGranule);
    const uintptr_t end = AlignUp((uintptr_t)addr + size, kGranule);
    pthread_mutex_lock(&g_windowLock);
    const int n = g_windowCount.load(std::memory_order_relaxed);
    uint32_t handle = 0;
    if (n < kMaxWindows && end - start <= (uintptr_t)(kEscapeBase - g_nextWindowHandle))
    {
        handle = g_nextWindowHandle;
        g_windows[n] = { start, handle, (uint32_t)(end - start) };
        g_nextWindowHandle += (uint32_t)(end - start);
        if (handle == kWindowBase)
        {
            // the first window (the executable image, RegisterImageWindow): also decoded inline (ptr32.h); set
            // before the arena is published, so before any other thread exists
            g_imageWindowStart = start;
            g_imageWindowSize = (uint32_t)(end - start);
        }
        g_windowCount.store(n + 1, std::memory_order_release);
    }
    pthread_mutex_unlock(&g_windowLock);
    return handle ? handle + (uint32_t)((uintptr_t)addr - start) : 0;
}

uint32_t EncodeSlow(const volatile void *p)
{
    const uintptr_t a = (uintptr_t)p;
    if (a < kSmallLimit)
        return (uint32_t)a; // null, small integers
    if ((intptr_t)a < 0 && (intptr_t)a >= (intptr_t)(int32_t)kNegativeBase)
        return (uint32_t)(intptr_t)a; // -1, -2 (loader markers), other small negative integers
    if (!g_arenaBase)
    {
        InitArena();
        const uintptr_t d = a - (uintptr_t)g_arenaBase;
        if (d - kSmallLimit < kArenaSize - kSmallLimit)
            return (uint32_t)d;
    }
    const int nw = g_windowCount.load(std::memory_order_acquire);
    for (int i = 0; i < nw; ++i)
    {
        const uintptr_t d = a - g_windows[i].addr;
        if (d < g_windows[i].size)
            return g_windows[i].handle + (uint32_t)d;
    }
    // escape table
    const uint32_t mask = (1u << kEscapeHashBits) - 1u;
    uint32_t h = (uint32_t)((a >> 3) * 0x9E3779B97F4A7C15ull >> (64 - kEscapeHashBits));
    pthread_mutex_lock(&g_escapeLock);
    for (;;)
    {
        const uintptr_t k = g_escapeHashKeys[h];
        if (k == a)
        {
            const uint32_t v = g_escapeHashValues[h];
            pthread_mutex_unlock(&g_escapeLock);
            return kEscapeBase + v;
        }
        if (k == 0)
            break;
        h = (h + 1) & mask;
    }
    const uint32_t idx = g_escapeCount.load(std::memory_order_relaxed);
    if (idx >= kEscapeCapacity || idx >= (1u << kEscapeHashBits) / 2)
    {
        pthread_mutex_unlock(&g_escapeLock);
        fprintf(stderr, "[arena] Ptr32 escape table exhausted\n");
        abort();
    }
    g_escapeTable[idx] = (void *)p;
    std::atomic_thread_fence(std::memory_order_release);
    g_escapeHashKeys[h] = a;
    g_escapeHashValues[h] = idx;
    g_escapeCount.store(idx + 1, std::memory_order_release);
    if (idx && (idx & (idx - 1)) == 0 && idx >= 4096)
        fprintf(stderr, "[arena] %u escaped pointers\n", idx);
    pthread_mutex_unlock(&g_escapeLock);
    return kEscapeBase + idx;
}

void *DecodeSlow(uint32_t v)
{
    if (v < kSmallLimit)
        return (void *)(uintptr_t)v;
    if (v >= kNegativeBase)
        return (void *)(intptr_t)(int32_t)v;
    if (v >= kEscapeBase)
    {
        const uint32_t idx = v - kEscapeBase;
        return idx < g_escapeCount.load(std::memory_order_acquire) ? g_escapeTable[idx] : nullptr;
    }
    if (v < kWindowBase) // arena handle before the arena exists: cannot have been produced by Encode
        return g_arenaBase ? g_arenaBase + v : nullptr;
    const int nw = g_windowCount.load(std::memory_order_acquire);
    for (int i = 0; i < nw; ++i)
    {
        const uint32_t d = v - g_windows[i].handle;
        if (d < g_windows[i].size)
            return (void *)(g_windows[i].addr + d);
    }
    return nullptr;
}
} // namespace bo1ptr

// ---------------------------------------------------------------------------------------------------------------
// Region allocator (VirtualAlloc backend)
// ---------------------------------------------------------------------------------------------------------------

void *Arena_Reserve(size_t size, bool commit)
{
    bo1ptr::InitArena();
    size = AlignUp(size ? size : 1, kGranule);
    pthread_mutex_lock(&g_regionLock);
    void *result = nullptr;
    for (auto it = g_free->begin(); it != g_free->end(); ++it)
    {
        const uintptr_t start = it->first, end = it->second;
        if (end - start >= size)
        {
            g_free->erase(it);
            if (end - start > size)
                (*g_free)[start + size] = end;
            (*g_reserved)[start] = size;
            result = bo1ptr::g_arenaBase + start;
            break;
        }
    }
    pthread_mutex_unlock(&g_regionLock);
    if (!result)
    {
        fprintf(stderr, "[arena] out of arena address space for a %zu-byte reservation; using system memory\n", size);
        return commit ? CommitAnywhere(size) : ReserveVirtual(size);
    }
    if (commit && mprotect(result, size, PROT_READ | PROT_WRITE) != 0)
    {
        Arena_Release(result);
        return nullptr;
    }
    return result;
}

bool Arena_Commit(void *addr, size_t size)
{
    const uintptr_t a = AlignDown((uintptr_t)addr, g_pageSize);
    const uintptr_t e = AlignUp((uintptr_t)addr + size, g_pageSize);
    return mprotect((void *)a, e - a, PROT_READ | PROT_WRITE) == 0;
}

bool Arena_Decommit(void *addr, size_t size)
{
    // Only whole pages inside the range lose their contents (and come back zeroed on the next commit).
    const uintptr_t a = AlignUp((uintptr_t)addr, g_pageSize);
    const uintptr_t e = AlignDown((uintptr_t)addr + size, g_pageSize);
    if (e <= a)
        return true;
    void *r = mmap((void *)a, e - a, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return r != MAP_FAILED;
}

bool Arena_Release(void *addr)
{
    if (!bo1ptr::InArena(addr))
        return false;
    const uintptr_t start = (uintptr_t)addr - (uintptr_t)bo1ptr::g_arenaBase;
    pthread_mutex_lock(&g_regionLock);
    auto it = g_reserved->find(start);
    if (it == g_reserved->end())
    {
        pthread_mutex_unlock(&g_regionLock);
        return false;
    }
    const uintptr_t size = it->second;
    g_reserved->erase(it);
    mmap(addr, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    // return to the free list, coalescing with neighbours
    uintptr_t s = start, e = start + size;
    auto next = g_free->lower_bound(s);
    if (next != g_free->end() && next->first == e)
    {
        e = next->second;
        next = g_free->erase(next);
    }
    if (next != g_free->begin())
    {
        auto prev = std::prev(next);
        if (prev->second == s)
        {
            s = prev->first;
            g_free->erase(prev);
        }
    }
    (*g_free)[s] = e;
    pthread_mutex_unlock(&g_regionLock);
    return true;
}

bool Arena_QueryReservation(const void *addr, void **base, size_t *size)
{
    if (!bo1ptr::InArena(addr))
        return false;
    const uintptr_t off = (uintptr_t)addr - (uintptr_t)bo1ptr::g_arenaBase;
    pthread_mutex_lock(&g_regionLock);
    auto it = g_reserved->upper_bound(off);
    bool found = false;
    if (it != g_reserved->begin())
    {
        --it;
        if (off < it->first + it->second)
        {
            *base = bo1ptr::g_arenaBase + it->first;
            *size = it->second;
            found = true;
        }
    }
    pthread_mutex_unlock(&g_regionLock);
    return found;
}

// ---------------------------------------------------------------------------------------------------------------
// Thread stacks
// ---------------------------------------------------------------------------------------------------------------

void *Arena_AllocStack(size_t size, size_t *usable)
{
    // one guard page below the stack catches overflows
    size = AlignUp(size, g_pageSize);
    char *p = (char *)Arena_Reserve(size + g_pageSize, true);
    if (!p)
        return nullptr;
    mprotect(p, g_pageSize, PROT_NONE);
    *usable = size;
    return p + g_pageSize;
}

void Arena_FreeStack(void *stack)
{
    if (stack)
        Arena_Release((char *)stack - g_pageSize);
}

// ---------------------------------------------------------------------------------------------------------------
// Heap inside the arena: the executable's malloc/free (arena_malloc.cpp), and C++ objects whose addresses are kept
// in Ptr32 fields (ArenaAllocated). Size classes up to 64 KB carved from 1 MB chunks; larger blocks get a region.
// Every block has a 16-byte header: { magic, class (or kLargeClass), size of a large block }.
// ---------------------------------------------------------------------------------------------------------------

namespace
{
constexpr size_t kChunk = 1 << 20;
constexpr uint32_t kHeapMagic = 0x50484231u;    // "1BHP"
constexpr uint32_t kAlignedMagic = 0x4C414231u; // "1BAL": header of an over-aligned block, points back at the block
constexpr uint32_t kLargeClass = 0xFFFFFFFFu;
constexpr size_t kMaxSmall = 65536;

struct BlockHeader
{
    uint32_t magic;
    uint32_t cls;
    uint64_t size; // large blocks: the region size; aligned blocks: offset back to the real block
};
static_assert(sizeof(BlockHeader) == 16, "16-byte block header");

// classes: 16..256 in steps of 16, then four per power of two up to 64 KB
constexpr int kSmallStepClasses = 16;
constexpr int kClasses = kSmallStepClasses + 8 * 4;
size_t g_classSize[kClasses];
bool g_classInit;

struct FreeNode
{
    FreeNode *next;
};
pthread_mutex_t g_heapLock = PTHREAD_MUTEX_INITIALIZER;
FreeNode *g_classFree[kClasses];
char *g_chunkPos;
char *g_chunkEnd;

void InitClasses()
{
    int c = 0;
    for (; c < kSmallStepClasses; ++c)
        g_classSize[c] = (size_t)(c + 1) * 16;
    for (size_t base = 256; c < kClasses; base <<= 1)
        for (int q = 1; q <= 4 && c < kClasses; ++q)
            g_classSize[c++] = base + base / 4 * q;
    g_classInit = true;
}

int SizeClass(size_t n)
{
    if (n <= 256)
        return n ? (int)((n - 1) >> 4) : 0;
    int c = kSmallStepClasses;
    while (c < kClasses && g_classSize[c] < n)
        ++c;
    return c < kClasses ? c : -1;
}
} // namespace

void *Arena_Malloc(size_t n)
{
    const size_t total = n + sizeof(BlockHeader);
    if (total < n)
        return nullptr;
    if (total > kMaxSmall)
    {
        const size_t sz = AlignUp(total, kGranule);
        BlockHeader *h = (BlockHeader *)Arena_Reserve(sz, true);
        if (!h)
            return nullptr;
        if (!bo1ptr::InArena(h))
        {
            // the arena is exhausted: the block came from system memory and cannot be told apart later
            fprintf(stderr, "[arena] heap exhausted\n");
            abort();
        }
        h->magic = kHeapMagic;
        h->cls = kLargeClass;
        h->size = sz;
        return h + 1;
    }
    pthread_mutex_lock(&g_heapLock);
    if (!g_classInit)
        InitClasses();
    const int c = SizeClass(total);
    char *p;
    if (g_classFree[c])
    {
        p = (char *)g_classFree[c];
        g_classFree[c] = g_classFree[c]->next;
    }
    else
    {
        const size_t sz = g_classSize[c];
        if (!g_chunkPos || g_chunkPos + sz > g_chunkEnd)
        {
            // the chunk's tail is left unused; small classes keep their own free lists
            char *chunk = (char *)Arena_Reserve(kChunk, true);
            if (!chunk || !bo1ptr::InArena(chunk))
            {
                pthread_mutex_unlock(&g_heapLock);
                fprintf(stderr, "[arena] heap exhausted\n");
                abort();
            }
            g_chunkPos = chunk;
            g_chunkEnd = chunk + kChunk;
        }
        p = g_chunkPos;
        g_chunkPos += sz;
    }
    pthread_mutex_unlock(&g_heapLock);
    BlockHeader *h = (BlockHeader *)p;
    h->magic = kHeapMagic;
    h->cls = (uint32_t)c;
    h->size = 0;
    return h + 1;
}

void *Arena_Calloc(size_t n)
{
    void *p = Arena_Malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

static BlockHeader *BlockOf(void *ptr)
{
    BlockHeader *h = (BlockHeader *)ptr - 1;
    if (h->magic == kAlignedMagic)
        h = (BlockHeader *)((char *)h - h->size);
    return h;
}

void Arena_Free(void *ptr)
{
    if (!ptr)
        return;
    BlockHeader *h = BlockOf(ptr);
    if (h->magic != kHeapMagic)
    {
        fprintf(stderr, "[arena] free of a pointer that is not a heap block: %p\n", ptr);
        abort();
    }
    if (h->cls == kLargeClass)
    {
        Arena_Release(h);
        return;
    }
    const uint32_t c = h->cls;
    h->magic = 0;
    pthread_mutex_lock(&g_heapLock);
    ((FreeNode *)h)->next = g_classFree[c];
    g_classFree[c] = (FreeNode *)h;
    pthread_mutex_unlock(&g_heapLock);
}

size_t Arena_UsableSize(void *ptr)
{
    BlockHeader *h = BlockOf(ptr);
    const size_t blockSize = h->cls == kLargeClass ? (size_t)h->size : g_classSize[h->cls];
    return blockSize - (size_t)((char *)ptr - (char *)h);
}

void *Arena_Memalign(size_t alignment, size_t n)
{
    if (alignment <= 16)
        return Arena_Malloc(n);
    // over-allocate, then put an aligned-block header right before the aligned address
    char *p = (char *)Arena_Malloc(n + alignment + sizeof(BlockHeader));
    if (!p)
        return nullptr;
    char *a = (char *)AlignUp((uintptr_t)p + sizeof(BlockHeader), alignment);
    BlockHeader *ah = (BlockHeader *)a - 1;
    BlockHeader *h = (BlockHeader *)p - 1;
    ah->magic = kAlignedMagic;
    ah->cls = 0;
    ah->size = (uint64_t)((char *)ah - (char *)h);
    return a;
}

void *Arena_Realloc(void *ptr, size_t n)
{
    if (!ptr)
        return Arena_Malloc(n);
    const size_t have = Arena_UsableSize(ptr);
    if (n <= have && (n >= have / 2 || have <= 256))
        return ptr;
    void *q = Arena_Malloc(n);
    if (!q)
        return nullptr;
    memcpy(q, ptr, n < have ? n : have);
    Arena_Free(ptr);
    return q;
}
