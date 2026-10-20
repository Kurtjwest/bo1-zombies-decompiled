// 64-bit Windows: the engine's memory where its 32-bit pointer handles reach it (src/universal/ptr32.h,
// src/platform/win32compat/arena.cpp).
//
//   - VirtualAlloc, VirtualFree and VirtualQuery: the executable's calls (the engine's hunk, fastfile blocks and
//     physical-memory pools, the statically linked libraries) come here through the link's --wrap
//     (cmake/windows.cmake). New reservations are carved out of the arena, as the compatibility layer's VirtualAlloc
//     does on macOS and Linux; anything else (addresses outside the arena) goes to the system unchanged.
//   - thread stacks: the system allocates every thread's stack anywhere in the address space, so each one is mapped
//     onto a range of window handles while its thread runs (a TLS callback: for the main thread when the process
//     starts, for every other thread when it starts and ends), and the engine's pointers to locals keep working.
//
// The malloc family is in arena_malloc.cpp.

#include <universal/ptr32.h>
#include <platform/win32compat/arena.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

#if !defined(_WIN64)
#error "win64_memory.cpp is for 64-bit Windows builds"
#endif

namespace
{
SIZE_T PageSize()
{
    static SIZE_T page;
    if (!page)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        page = si.dwPageSize;
    }
    return page;
}

// ---- thread stacks as windows ----

// [low, high): the thread's whole stack reservation (the committed part grows down inside it), as its TEB describes
// it (the loader may run the TLS callbacks on a stack of its own: Wine does at process start)
bool CurrentStack(char **low, char **high)
{
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    MEMORY_BASIC_INFORMATION mbi;
    if (!tib->StackBase ||
        !VirtualQueryEx(GetCurrentProcess(), (const char *)tib->StackBase - 1, &mbi, sizeof(mbi)) || !mbi.AllocationBase)
        return false;
    *low = (char *)mbi.AllocationBase;
    *high = (char *)tib->StackBase;
    return *high > *low;
}

void RegisterThreadStack()
{
    // (the arena registers the executable image's window first: it is decoded inline, ptr32.h)
    bo1ptr::InitArena();
    char *low, *high;
    if (CurrentStack(&low, &high))
        bo1ptr::RegisterWindow(low, (size_t)(high - low));
}

void UnregisterThreadStack()
{
    char *low, *high;
    if (CurrentStack(&low, &high))
        bo1ptr::UnregisterWindow(low);
}

void NTAPI StackWindowsTlsCallback(PVOID, DWORD reason, PVOID)
{
    if (reason == DLL_PROCESS_ATTACH || reason == DLL_THREAD_ATTACH)
        RegisterThreadStack();
    else if (reason == DLL_THREAD_DETACH)
        UnregisterThreadStack();
}
} // namespace

// The loader runs the image's TLS callbacks (the .CRT$XL* array, MinGW's tlssup.c) for the main thread before the
// entry point and for every thread that starts or ends.
extern "C" const IMAGE_TLS_DIRECTORY _tls_used;
extern "C" __attribute__((section(".CRT$XLF"), used)) const PIMAGE_TLS_CALLBACK bo1_stackWindowsTlsCallback = StackWindowsTlsCallback;
// (keeps the TLS directory in the image)
extern "C" __attribute__((used)) const void *const bo1_tlsUsed = &_tls_used;

// ---- VirtualAlloc, VirtualFree, VirtualQuery ----

extern "C" {

LPVOID WINAPI __wrap_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect)
{
    const HANDLE self = GetCurrentProcess();
    if (!size)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    // New read/write memory: a reservation in the arena (committed too when asked; MEM_COMMIT without an address
    // reserves as well, as Windows does). A requested address inside the arena is only a hint, as on macOS and Linux.
    if (!(type & ~(MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN)) && (type & (MEM_RESERVE | MEM_COMMIT)) &&
        (protect == PAGE_READWRITE || protect == PAGE_NOACCESS) &&
        (!addr || ((type & MEM_RESERVE) && bo1ptr::InArena(addr))))
    {
        void *p = Arena_Reserve(size, (type & MEM_COMMIT) != 0);
        if (!p)
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return p;
    }
    if (addr && bo1ptr::InArena(addr) && (type & MEM_COMMIT) && !(type & MEM_RESERVE))
    {
        if (!Arena_Commit(addr, size))
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        if (protect != PAGE_READWRITE)
        {
            DWORD old;
            VirtualProtectEx(self, addr, size, protect, &old);
        }
        return addr;
    }
    // anything else: the system's (executable memory, MEM_RESET, addresses outside the arena)
    return VirtualAllocEx(self, addr, size, type, protect);
}

BOOL WINAPI __wrap_VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    if (addr && bo1ptr::InArena(addr))
    {
        if (type & MEM_RELEASE)
        {
            if (size || !Arena_Release(addr))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            return TRUE;
        }
        if (type & MEM_DECOMMIT)
        {
            void *base;
            size_t reserved;
            if (!Arena_QueryReservation(addr, &base, &reserved))
            {
                SetLastError(ERROR_INVALID_ADDRESS);
                return FALSE;
            }
            // size 0: the whole reservation from addr on
            if (!size)
                size = (SIZE_T)((char *)base + reserved - (char *)addr);
            return Arena_Decommit(addr, size) ? TRUE : FALSE;
        }
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return VirtualFreeEx(GetCurrentProcess(), addr, size, type);
}

// Inside the arena a reservation is what VirtualAlloc handed out, not the arena: AllocationBase and RegionSize describe
// that reservation, and the space between reservations is free.
SIZE_T WINAPI __wrap_VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION mbi, SIZE_T len)
{
    const SIZE_T n = VirtualQueryEx(GetCurrentProcess(), addr, mbi, len);
    if (!n || !bo1ptr::InArena(addr))
        return n;
    void *base;
    size_t reserved;
    const uintptr_t page = (uintptr_t)addr & ~(uintptr_t)(PageSize() - 1);
    if (Arena_QueryReservation(addr, &base, &reserved))
    {
        mbi->AllocationBase = base;
        mbi->AllocationProtect = PAGE_READWRITE;
        const uintptr_t end = (uintptr_t)base + reserved;
        if ((uintptr_t)mbi->BaseAddress + mbi->RegionSize > end)
            mbi->RegionSize = end - (uintptr_t)mbi->BaseAddress;
    }
    else
    {
        mbi->BaseAddress = (PVOID)page;
        mbi->AllocationBase = nullptr;
        mbi->AllocationProtect = 0;
        mbi->RegionSize = PageSize();
        mbi->State = MEM_FREE;
        mbi->Protect = PAGE_NOACCESS;
        mbi->Type = 0;
    }
    return n;
}

} // extern "C"
