// The 32-bit handle space behind Ptr32 (src/universal/ptr32.h) and the memory it covers.
//
// The engine was written for (and decompiled from) a 32-bit program: asset structures keep 32-bit pointer fields,
// and plenty of code keeps addresses in 32-bit integers and does arithmetic on them. A 64-bit macOS process cannot
// map anything below 4 GB (Linux keeps the low 4 GB for the executable and its first mappings, not for us either, and
// 64-bit Windows hands out memory anywhere too), so instead every address the engine produces is given a 32-bit
// handle such that handle arithmetic is address arithmetic:
//   - the arena: one contiguous 3.25 GB reservation. VirtualAlloc (hunk, fastfile blocks, PMem), the C heap of the
//     executable (arena_malloc.cpp) and, on macOS and Linux, the stacks of CreateThread threads are carved out of
//     it; a handle is the offset from its base.
//   - windows: the executable image (globals, string literals, code) and the main thread's stack (on Windows every
//     thread's stack: src/platform/windows/win64_memory.cpp), mapped linearly onto a handle range of their own.
//   - an escape table for any other address (system heaps, other images): such handles round-trip exactly.
// See the handle layout in ptr32.h.

#include <universal/ptr32.h>
#include "arena.h"

#if defined(_WIN32)
// 64-bit Windows (cmake/windows.cmake): the same arena, reserved with VirtualAlloc; the executable's malloc family and
// VirtualAlloc calls are routed into it (arena_malloc.cpp, src/platform/windows/win64_memory.cpp)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#else
#include <link.h>
#endif
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#endif
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>

#if defined(__APPLE__)
extern "C" const struct mach_header_64 _mh_execute_header;
#endif

namespace
{
// Locks usable before the C runtime has started: on Windows the arena serves the runtime's own first allocations.
#if defined(_WIN32)
struct ArenaLock
{
    SRWLOCK l = SRWLOCK_INIT;
    void lock() { AcquireSRWLockExclusive(&l); }
    void unlock() { ReleaseSRWLockExclusive(&l); }
};
inline void YieldThread() { SwitchToThread(); }
size_t SystemPageSize()
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
}
#else
struct ArenaLock
{
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    void lock() { pthread_mutex_lock(&m); }
    void unlock() { pthread_mutex_unlock(&m); }
};
inline void YieldThread() { sched_yield(); }
size_t SystemPageSize() { return (size_t)sysconf(_SC_PAGESIZE); }
#endif

// The bookkeeping maps: on Windows their nodes come from the process heap, never from the arena's own heap (all of
// the executable's allocations land there, these maps' own included otherwise, while the region lock is held). On
// macOS and Linux the C++ allocator is the system's already.
#if defined(_WIN32)
template <typename T> struct SystemAllocator
{
    using value_type = T;
    SystemAllocator() = default;
    template <typename U> SystemAllocator(const SystemAllocator<U> &) {}
    T *allocate(size_t n)
    {
        void *p = HeapAlloc(GetProcessHeap(), 0, n * sizeof(T));
        if (!p)
            abort();
        return (T *)p;
    }
    void deallocate(T *p, size_t) { HeapFree(GetProcessHeap(), 0, p); }
    template <typename U> bool operator==(const SystemAllocator<U> &) const { return true; }
    template <typename U> bool operator!=(const SystemAllocator<U> &) const { return false; }
};
using RangeMap = std::map<uintptr_t, uintptr_t, std::less<uintptr_t>, SystemAllocator<std::pair<const uintptr_t, uintptr_t>>>;
#else
using RangeMap = std::map<uintptr_t, uintptr_t>;
#endif

// ---- address space ----
#if defined(_WIN32)
// (the Ex calls: the executable's VirtualAlloc, VirtualFree and VirtualQuery come back here, win64_memory.cpp)
void *ReserveVirtual(size_t size) { return VirtualAllocEx(GetCurrentProcess(), nullptr, size, MEM_RESERVE, PAGE_NOACCESS); }
void *CommitAnywhere(size_t size)
{
    return VirtualAllocEx(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
bool CommitPages(void *addr, size_t size)
{
    return VirtualAllocEx(GetCurrentProcess(), addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}
// the pages read back as zero when they are committed again
bool DecommitPages(void *addr, size_t size) { return VirtualFreeEx(GetCurrentProcess(), addr, size, MEM_DECOMMIT) != 0; }
void ProtectNoAccess(void *addr, size_t size)
{
    DWORD old;
    VirtualProtectEx(GetCurrentProcess(), addr, size, PAGE_NOACCESS, &old);
}
#else
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
bool CommitPages(void *addr, size_t size) { return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0; }
bool DecommitPages(void *addr, size_t size)
{
    return mmap(addr, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0) != MAP_FAILED;
}
void ProtectNoAccess(void *addr, size_t size) { mprotect(addr, size, PROT_NONE); }
#endif
} // namespace

#if BO1_PTR32_IDENTITY
// ---------------------------------------------------------------------------------------------------------------
// 32-bit processes (x86, 32-bit ARM): every address is its own 32-bit handle (ptr32.h), so there is no arena to
// reserve: the regions below are mapped anywhere, and the heap further down carves its blocks out of them.
// ---------------------------------------------------------------------------------------------------------------
namespace
{
constexpr uintptr_t kGranule = 0x10000; // Windows allocation granularity
constinit ArenaLock g_regionLock;
RangeMap *g_reserved; // live reservations: start -> size
size_t g_pageSize = 4096;

uintptr_t AlignUp(uintptr_t v, uintptr_t a) { return (v + a - 1) & ~(a - 1); }
uintptr_t AlignDown(uintptr_t v, uintptr_t a) { return v & ~(a - 1); }
} // namespace

namespace bo1ptr
{
void InitArena()
{
    static std::atomic<int> once{0};
    int expected = 0;
    if (once.compare_exchange_strong(expected, 1))
    {
        g_pageSize = SystemPageSize();
        alignas(RangeMap) static unsigned char reservedStorage[sizeof(RangeMap)];
        g_reserved = new (reservedStorage) RangeMap();
        once.store(2, std::memory_order_release);
    }
    else
    {
        while (once.load(std::memory_order_acquire) != 2)
            YieldThread();
    }
}

bool InArena(const volatile void *) { return true; }

uint32_t RegisterWindow(const void *addr, size_t) { return (uint32_t)(uintptr_t)addr; }
void UnregisterWindow(const void *) {}
} // namespace bo1ptr

void *Arena_Reserve(size_t size, bool commit)
{
    bo1ptr::InitArena();
    size = AlignUp(size ? size : 1, kGranule);
    // 64 KB aligned, as VirtualAlloc's reservations are: map a granule more and trim
    char *raw = (char *)mmap(nullptr, size + kGranule, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED)
        return nullptr;
    char *base = (char *)AlignUp((uintptr_t)raw, kGranule);
    if (base > raw)
        munmap(raw, (size_t)(base - raw));
    const size_t tail = (size_t)(raw + size + kGranule - (base + size));
    if (tail)
        munmap(base + size, tail);
    if (commit && mprotect(base, size, PROT_READ | PROT_WRITE) != 0)
    {
        munmap(base, size);
        return nullptr;
    }
    g_regionLock.lock();
    (*g_reserved)[(uintptr_t)base] = size;
    g_regionLock.unlock();
    return base;
}

bool Arena_Commit(void *addr, size_t size)
{
    const uintptr_t a = AlignDown((uintptr_t)addr, g_pageSize);
    const uintptr_t e = AlignUp((uintptr_t)addr + size, g_pageSize);
    return mprotect((void *)a, e - a, PROT_READ | PROT_WRITE) == 0;
}

bool Arena_Decommit(void *addr, size_t size)
{
    const uintptr_t a = AlignUp((uintptr_t)addr, g_pageSize);
    const uintptr_t e = AlignDown((uintptr_t)addr + size, g_pageSize);
    if (e <= a)
        return true;
    void *r = mmap((void *)a, e - a, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return r != MAP_FAILED;
}

bool Arena_Release(void *addr)
{
    bo1ptr::InitArena();
    g_regionLock.lock();
    auto it = g_reserved->find((uintptr_t)addr);
    if (it == g_reserved->end())
    {
        g_regionLock.unlock();
        return false;
    }
    const size_t size = it->second;
    g_reserved->erase(it);
    g_regionLock.unlock();
    munmap(addr, size);
    return true;
}

bool Arena_QueryReservation(const void *addr, void **base, size_t *size)
{
    bo1ptr::InitArena();
    const uintptr_t a = (uintptr_t)addr;
    g_regionLock.lock();
    bool found = false;
    auto it = g_reserved->upper_bound(a);
    if (it != g_reserved->begin())
    {
        --it;
        if (a < it->first + it->second)
        {
            *base = (void *)it->first;
            *size = it->second;
            found = true;
        }
    }
    g_regionLock.unlock();
    return found;
}

#else // !BO1_PTR32_IDENTITY: 64-bit processes, the arena

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

// (constinit, here and below: the arena is in use before the program's static constructors run, from the first malloc
// or a TLS callback on Windows, so none of its state may have a constructor that would reset it afterwards)
constinit std::atomic<int> g_arenaInitState{0}; // 0 = no, 1 = in progress, 2 = done
constinit ArenaLock g_regionLock;

// free ranges [start, end) as offsets from the arena base, keyed by start
RangeMap *g_free;
// live reservations: start -> size
RangeMap *g_reserved;
size_t g_pageSize = 16384;

// windows: [addr, addr + size) <-> [handle, handle + size). A slot keeps its handle range for good; a window that
// goes away (an exited thread's stack, UnregisterWindow) leaves the slot free for the next one that fits in it.
// Readers take no lock: a slot that changes is bracketed by g_windowSeq going odd and even again (a sequence lock),
// and new slots are published through g_windowCount.
struct Window
{
    std::atomic<uintptr_t> addr{0};
    std::atomic<uint32_t> size{0}; // 0: free
    uint32_t handle = 0;           // first handle of the slot (fixed)
    uint32_t capacity = 0;         // handles the slot owns (fixed)
};
#if defined(_WIN32)
// every thread's stack is a window there (src/platform/windows/win64_memory.cpp)
constexpr int kMaxWindows = 1024;
#else
constexpr int kMaxWindows = 16;
#endif
constinit Window g_windows[kMaxWindows];
constinit std::atomic<int> g_windowCount{0};
constinit std::atomic<uint32_t> g_windowSeq{0};
constinit uint32_t g_nextWindowHandle = bo1ptr::kWindowBase;
constinit ArenaLock g_windowLock;

// escape table
constinit ArenaLock g_escapeLock;
constinit std::atomic<uint32_t> g_escapeCount{0};
void *volatile *g_escapeTable; // escape index -> address
uintptr_t *g_escapeHashKeys;   // address, 0 = empty
uint32_t *g_escapeHashValues;  // escape index

// ---- windows: lock-free lookups ----
bool WindowHandleOf(uintptr_t a, uint32_t *handle)
{
    for (;;)
    {
        const uint32_t seq = g_windowSeq.load(std::memory_order_acquire);
        if (!(seq & 1))
        {
            bool found = false;
            uint32_t h = 0;
            const int nw = g_windowCount.load(std::memory_order_acquire);
            for (int i = 0; i < nw; ++i)
            {
                const uintptr_t d = a - g_windows[i].addr.load(std::memory_order_relaxed);
                if (d < g_windows[i].size.load(std::memory_order_relaxed))
                {
                    h = g_windows[i].handle + (uint32_t)d;
                    found = true;
                    break;
                }
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (g_windowSeq.load(std::memory_order_relaxed) == seq)
            {
                *handle = h;
                return found;
            }
        }
        YieldThread();
    }
}

bool WindowAddressOf(uint32_t v, uintptr_t *addr)
{
    for (;;)
    {
        const uint32_t seq = g_windowSeq.load(std::memory_order_acquire);
        if (!(seq & 1))
        {
            bool found = false;
            uintptr_t r = 0;
            const int nw = g_windowCount.load(std::memory_order_acquire);
            for (int i = 0; i < nw; ++i)
            {
                const uint32_t d = v - g_windows[i].handle;
                if (d < g_windows[i].size.load(std::memory_order_relaxed))
                {
                    r = g_windows[i].addr.load(std::memory_order_relaxed) + d;
                    found = true;
                    break;
                }
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (g_windowSeq.load(std::memory_order_relaxed) == seq)
            {
                *addr = r;
                return found;
            }
        }
        YieldThread();
    }
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

#if defined(_WIN32)
// The executable's image: its headers and sections (code, read-only data, data and bss), as the loader mapped them,
// without the discardable ones at its end (relocations, and the DWARF debug information a MinGW build keeps there).
void RegisterImageWindow()
{
    const char *base = (const char *)GetModuleHandleW(nullptr);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t end = nt->OptionalHeader.SizeOfHeaders;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_DISCARDABLE)
            continue;
        const uintptr_t size = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        if (sec[i].VirtualAddress + size > end)
            end = sec[i].VirtualAddress + size;
    }
    if (!bo1ptr::RegisterWindow(base, end))
        fprintf(stderr, "[arena] cannot map the executable image (%zu bytes) to handles\n", (size_t)end);
}
#elif defined(__APPLE__)
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
#else
// The main executable's loadable segments (text, read-only data, data and bss), from its ELF program headers.
int ImageSegments(struct dl_phdr_info *info, size_t, void *data)
{
    uintptr_t *lohi = (uintptr_t *)data;
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) &ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || !ph.p_memsz)
            continue;
        const uintptr_t lo = info->dlpi_addr + ph.p_vaddr, hi = lo + ph.p_memsz;
        lohi[0] = lo < lohi[0] ? lo : lohi[0];
        lohi[1] = hi > lohi[1] ? hi : lohi[1];
    }
    return 1; // the main program is the first object dl_iterate_phdr reports
}

void RegisterImageWindow()
{
    uintptr_t lohi[2] = {UINTPTR_MAX, 0};
    dl_iterate_phdr(ImageSegments, lohi);
    if (lohi[0] < lohi[1] && !bo1ptr::RegisterWindow((const void *)lohi[0], lohi[1] - lohi[0]))
        fprintf(stderr, "[arena] cannot map the executable image (%zu bytes) to handles\n", (size_t)(lohi[1] - lohi[0]));
}
#endif
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
            YieldThread();
        return;
    }
    g_pageSize = SystemPageSize();
    // Reserve the arena plus one granule of slack so the base can be aligned.
    const size_t want = (size_t)kArenaSize + kGranule;
    char *raw = (char *)ReserveVirtual(want);
    if (!raw)
    {
        fprintf(stderr, "[arena] cannot reserve %zu bytes of address space\n", want);
        abort();
    }
    char *base = (char *)AlignUp((uintptr_t)raw, kGranule);
    // the maps' nodes come from the system heap (RangeMap), never from the arena's own malloc
    alignas(RangeMap) static unsigned char freeStorage[sizeof(RangeMap)], reservedStorage[sizeof(RangeMap)];
    g_free = new (freeStorage) RangeMap();
    g_reserved = new (reservedStorage) RangeMap();
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
    if (end - start > (uintptr_t)(kEscapeBase - kWindowBase))
        return 0;
    const uint32_t need = (uint32_t)(end - start);
    g_windowLock.lock();
    const int n = g_windowCount.load(std::memory_order_relaxed);
    // the smallest free slot it fits in
    int slot = -1;
    for (int i = 0; i < n; ++i)
        if (!g_windows[i].size.load(std::memory_order_relaxed) && g_windows[i].capacity >= need &&
            (slot < 0 || g_windows[i].capacity < g_windows[slot].capacity))
            slot = i;
    uint32_t handle = 0;
    if (slot >= 0)
    {
        Window &w = g_windows[slot];
        handle = w.handle;
        g_windowSeq.fetch_add(1, std::memory_order_relaxed); // odd: readers retry
        std::atomic_thread_fence(std::memory_order_release);
        w.addr.store(start, std::memory_order_relaxed);
        w.size.store(need, std::memory_order_relaxed);
        g_windowSeq.fetch_add(1, std::memory_order_release);
    }
    else if (n < kMaxWindows && need <= (uint32_t)(kEscapeBase - g_nextWindowHandle))
    {
        Window &w = g_windows[n];
        handle = g_nextWindowHandle;
        w.handle = handle;
        w.capacity = need;
        w.addr.store(start, std::memory_order_relaxed);
        w.size.store(need, std::memory_order_relaxed);
        g_nextWindowHandle += need;
        if (handle == kWindowBase)
        {
            // the first window (the executable image, RegisterImageWindow): also decoded inline (ptr32.h); set
            // before the arena is published, so before any other thread exists
            g_imageWindowStart = start;
            g_imageWindowSize = need;
        }
        g_windowCount.store(n + 1, std::memory_order_release);
    }
    g_windowLock.unlock();
    return handle ? handle + (uint32_t)((uintptr_t)addr - start) : 0;
}

void UnregisterWindow(const void *addr)
{
    const uintptr_t start = AlignDown((uintptr_t)addr, kGranule);
    g_windowLock.lock();
    const int n = g_windowCount.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        Window &w = g_windows[i];
        // (the image window, the first, stays)
        if (w.handle != kWindowBase && w.size.load(std::memory_order_relaxed) && w.addr.load(std::memory_order_relaxed) == start)
        {
            g_windowSeq.fetch_add(1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            w.size.store(0, std::memory_order_relaxed);
            g_windowSeq.fetch_add(1, std::memory_order_release);
            break;
        }
    }
    g_windowLock.unlock();
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
    uint32_t windowHandle;
    if (WindowHandleOf(a, &windowHandle))
        return windowHandle;
    // escape table
    const uint32_t mask = (1u << kEscapeHashBits) - 1u;
    uint32_t h = (uint32_t)((a >> 3) * 0x9E3779B97F4A7C15ull >> (64 - kEscapeHashBits));
    g_escapeLock.lock();
    for (;;)
    {
        const uintptr_t k = g_escapeHashKeys[h];
        if (k == a)
        {
            const uint32_t v = g_escapeHashValues[h];
            g_escapeLock.unlock();
            return kEscapeBase + v;
        }
        if (k == 0)
            break;
        h = (h + 1) & mask;
    }
    const uint32_t idx = g_escapeCount.load(std::memory_order_relaxed);
    if (idx >= kEscapeCapacity || idx >= (1u << kEscapeHashBits) / 2)
    {
        g_escapeLock.unlock();
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
    g_escapeLock.unlock();
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
    uintptr_t windowAddr;
    return WindowAddressOf(v, &windowAddr) ? (void *)windowAddr : nullptr;
}
} // namespace bo1ptr

// ---------------------------------------------------------------------------------------------------------------
// Region allocator (VirtualAlloc backend)
// ---------------------------------------------------------------------------------------------------------------

void *Arena_Reserve(size_t size, bool commit)
{
    bo1ptr::InitArena();
    size = AlignUp(size ? size : 1, kGranule);
    g_regionLock.lock();
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
    g_regionLock.unlock();
    if (!result)
    {
        fprintf(stderr, "[arena] out of arena address space for a %zu-byte reservation; using system memory\n", size);
        return commit ? CommitAnywhere(size) : ReserveVirtual(size);
    }
    if (commit && !CommitPages(result, size))
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
    return CommitPages((void *)a, e - a);
}

bool Arena_Decommit(void *addr, size_t size)
{
    // Only whole pages inside the range lose their contents (and come back zeroed on the next commit).
    const uintptr_t a = AlignUp((uintptr_t)addr, g_pageSize);
    const uintptr_t e = AlignDown((uintptr_t)addr + size, g_pageSize);
    if (e <= a)
        return true;
    return DecommitPages((void *)a, e - a);
}

bool Arena_Release(void *addr)
{
    if (!bo1ptr::InArena(addr))
        return false;
    const uintptr_t start = (uintptr_t)addr - (uintptr_t)bo1ptr::g_arenaBase;
    g_regionLock.lock();
    auto it = g_reserved->find(start);
    if (it == g_reserved->end())
    {
        g_regionLock.unlock();
        return false;
    }
    const uintptr_t size = it->second;
    g_reserved->erase(it);
    DecommitPages(addr, size);
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
    g_regionLock.unlock();
    return true;
}

bool Arena_QueryReservation(const void *addr, void **base, size_t *size)
{
    if (!bo1ptr::InArena(addr))
        return false;
    const uintptr_t off = (uintptr_t)addr - (uintptr_t)bo1ptr::g_arenaBase;
    g_regionLock.lock();
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
    g_regionLock.unlock();
    return found;
}

#endif // BO1_PTR32_IDENTITY

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
    ProtectNoAccess(p, g_pageSize);
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
constinit ArenaLock g_heapLock;
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
    g_heapLock.lock();
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
                g_heapLock.unlock();
                fprintf(stderr, "[arena] heap exhausted\n");
                abort();
            }
            g_chunkPos = chunk;
            g_chunkEnd = chunk + kChunk;
        }
        p = g_chunkPos;
        g_chunkPos += sz;
    }
    g_heapLock.unlock();
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
    g_heapLock.lock();
    ((FreeNode *)h)->next = g_classFree[c];
    g_classFree[c] = (FreeNode *)h;
    g_heapLock.unlock();
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
