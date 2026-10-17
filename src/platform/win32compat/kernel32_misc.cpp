// Win32 compatibility layer for the macOS port: virtual memory (backed by the Ptr32 arena), heaps, time, system
// information, process/environment, modules, string conversion, registry stubs, crash handling and assorted APIs.

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <io.h>
#include "kernel32_internal.h"
#include "arena.h"
#include "main_thread.h"
#include <universal/ptr32.h>

#include <dlfcn.h>
#include <execinfo.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern char **environ;

using namespace w32;

namespace w32
{
static std::string g_commandLine;
static std::vector<std::wstring> g_wideCommandLine;

// The folder the game runs in (the original game's data: zone/, main/, ...). GetModuleFileName(NULL) reports the
// executable as living there, the way the Windows build's exe sits in its run folder: the engine derives its
// install path, log and semaphore names from the exe path.
std::string g_gameDir;
void SetGameDirectory(const char *dir) { g_gameDir = dir ? dir : ""; }

void SetCommandLine(int argc, char **argv)
{
    g_commandLine.clear();
    for (int i = 0; i < argc; ++i)
    {
        if (i)
            g_commandLine += ' ';
        bool quote = strchr(argv[i], ' ') != nullptr;
        if (quote)
            g_commandLine += '"';
        g_commandLine += argv[i];
        if (quote)
            g_commandLine += '"';
    }
}
} // namespace w32

namespace
{
mach_timebase_info_data_t g_timebase;
uint64_t g_startTicks;

void InitTime()
{
    if (!g_timebase.denom)
    {
        mach_timebase_info(&g_timebase);
        g_startTicks = mach_absolute_time();
    }
}

uint64_t MonotonicNs()
{
    InitTime();
    return (mach_absolute_time() - g_startTicks) * g_timebase.numer / g_timebase.denom;
}

void TmToSystemTime(const struct tm &tm, int ms, LPSYSTEMTIME st)
{
    st->wYear = (WORD)(tm.tm_year + 1900);
    st->wMonth = (WORD)(tm.tm_mon + 1);
    st->wDayOfWeek = (WORD)tm.tm_wday;
    st->wDay = (WORD)tm.tm_mday;
    st->wHour = (WORD)tm.tm_hour;
    st->wMinute = (WORD)tm.tm_min;
    st->wSecond = (WORD)tm.tm_sec;
    st->wMilliseconds = (WORD)ms;
}

constexpr unsigned long long kFileTimeUnixEpoch = 116444736000000000ull;

LPTOP_LEVEL_EXCEPTION_FILTER g_unhandledFilter;
std::atomic<int> g_inCrash{0};

void CrashSignalHandler(int sig, siginfo_t *info, void *uctx)
{
    if (g_inCrash.exchange(1))
        _exit(128 + sig);
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    memset(&rec, 0, sizeof(rec));
    memset(&ctx, 0, sizeof(ctx));
    switch (sig)
    {
    case SIGSEGV:
    case SIGBUS: rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION; break;
    case SIGILL: rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION; break;
    case SIGFPE: rec.ExceptionCode = EXCEPTION_FLT_INVALID_OPERATION; break;
    case SIGTRAP: rec.ExceptionCode = EXCEPTION_BREAKPOINT; break;
    default: rec.ExceptionCode = EXCEPTION_NONCONTINUABLE_EXCEPTION; break;
    }
    rec.ExceptionAddress = info ? info->si_addr : nullptr;
    rec.NumberParameters = 2;
    rec.ExceptionInformation[1] = (ULONG_PTR)(info ? info->si_addr : nullptr);
    ucontext_t *uc = (ucontext_t *)uctx;
    if (uc && uc->uc_mcontext)
    {
        ctx.Pc = (DWORD64)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
        ctx.Sp = (DWORD64)__darwin_arm_thread_state64_get_sp(uc->uc_mcontext->__ss);
        ctx.Fp = (DWORD64)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
        ctx.Lr = (DWORD64)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
        for (int i = 0; i < 29; ++i)
            ctx.X[i] = uc->uc_mcontext->__ss.__x[i];
        ctx.Eip = (DWORD_PTR)ctx.Pc;
        ctx.Esp = (DWORD_PTR)ctx.Sp;
        ctx.Ebp = (DWORD_PTR)ctx.Fp;
        ctx.Eax = (DWORD_PTR)ctx.X[0];
        ctx.Ebx = (DWORD_PTR)ctx.X[1];
        ctx.Ecx = (DWORD_PTR)ctx.X[2];
        ctx.Edx = (DWORD_PTR)ctx.X[3];
        ctx.Esi = (DWORD_PTR)ctx.X[4];
        ctx.Edi = (DWORD_PTR)ctx.X[5];
        ctx.EFlags = (DWORD)uc->uc_mcontext->__ss.__cpsr;
        ctx.ContextFlags = CONTEXT_FULL;
        rec.ExceptionAddress = (PVOID)ctx.Pc;
    }
    fprintf(stderr, "\n*** fatal signal %d at pc=%p addr=%p ***\n", sig, (void *)ctx.Pc, info ? info->si_addr : nullptr);
    void *frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
    if (g_unhandledFilter)
    {
        EXCEPTION_POINTERS ep = {&rec, &ctx};
        g_unhandledFilter(&ep);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void InstallCrashHandlers()
{
    static bool installed;
    if (installed)
        return;
    installed = true;
    static char altstack[256 * 1024];
    stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = CrashSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

std::string ExecutablePath()
{
    char buf[PATH_MAX];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) != 0)
        return "BO1Zombies";
    char real[PATH_MAX];
    if (realpath(buf, real))
        return real;
    return buf;
}
} // namespace

extern "C" {

// ---------------------------------------------------------------- virtual memory
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect)
{
    (void)protect;
    if (!size)
    {
        SetError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (addr && !(type & MEM_RESERVE))
    {
        // commit inside an existing reservation
        if (type & MEM_COMMIT)
        {
            if (!Arena_Commit(addr, size))
            {
                SetError(ERROR_NOT_ENOUGH_MEMORY);
                return nullptr;
            }
            return addr;
        }
        if (type & MEM_RESET)
            return addr;
        SetError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    // new reservation (a requested address is only a hint here)
    void *p = Arena_Reserve(size, (type & MEM_COMMIT) != 0);
    if (!p)
        SetError(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    if (!addr)
        return FALSE;
    if (type & MEM_RELEASE)
    {
        if (Arena_Release(addr))
            return TRUE;
        // not an arena reservation (system-memory fallback): unmap what we can
        void *base;
        size_t sz;
        if (Arena_QueryReservation(addr, &base, &sz))
            return FALSE;
        return size ? munmap(addr, size) == 0 : TRUE;
    }
    if (type & MEM_DECOMMIT)
        return Arena_Decommit(addr, size) ? TRUE : FALSE;
    return FALSE;
}
SIZE_T VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION mbi, SIZE_T len)
{
    if (!mbi || len < sizeof(*mbi))
        return 0;
    memset(mbi, 0, sizeof(*mbi));
    void *base;
    size_t size;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(page - 1);
    mbi->BaseAddress = (PVOID)a;
    if (Arena_QueryReservation(addr, &base, &size))
    {
        mbi->AllocationBase = base;
        mbi->AllocationProtect = PAGE_READWRITE;
        mbi->RegionSize = (uintptr_t)base + size - a;
        mbi->Type = MEM_PRIVATE;
        // committed pages are resident-capable; mincore tells us whether the page is accessible at all
        char vec = 0;
        bool committed = mincore((void *)a, page, &vec) == 0 || true;
        mbi->State = committed ? MEM_COMMIT : MEM_RESERVE;
        mbi->Protect = PAGE_READWRITE;
        return sizeof(*mbi);
    }
    // outside the arena: describe the enclosing VM region
    mach_vm_address_t raddr = a;
    mach_vm_size_t rsize = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj;
    if (mach_vm_region(mach_task_self(), &raddr, &rsize, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj) == KERN_SUCCESS && raddr <= a)
    {
        mbi->AllocationBase = (PVOID)raddr;
        mbi->RegionSize = (SIZE_T)(raddr + rsize - a);
        mbi->State = MEM_COMMIT;
        mbi->Type = MEM_PRIVATE;
        mbi->Protect = (info.protection & VM_PROT_WRITE) ? PAGE_READWRITE : (info.protection & VM_PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
        mbi->AllocationProtect = mbi->Protect;
        return sizeof(*mbi);
    }
    mbi->State = MEM_FREE;
    mbi->RegionSize = page;
    return sizeof(*mbi);
}
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old)
{
    if (old)
        *old = PAGE_READWRITE;
    int p = PROT_NONE;
    if (prot & (PAGE_READONLY | PAGE_EXECUTE_READ))
        p = PROT_READ;
    if (prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))
        p = PROT_READ | PROT_WRITE;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(page - 1);
    uintptr_t e = ((uintptr_t)addr + size + page - 1) & ~(uintptr_t)(page - 1);
    return mprotect((void *)a, e - a, p) == 0;
}
BOOL VirtualLock(LPVOID addr, SIZE_T size) { return mlock(addr, size) == 0; }
BOOL VirtualUnlock(LPVOID addr, SIZE_T size) { return munlock(addr, size) == 0; }

// ---------------------------------------------------------------- heaps (system malloc)
static int g_processHeapTag;
HANDLE GetProcessHeap(void) { return (HANDLE)&g_processHeapTag; }
HANDLE HeapCreate(DWORD, SIZE_T, SIZE_T) { return (HANDLE)&g_processHeapTag; }
BOOL HeapDestroy(HANDLE) { return TRUE; }
LPVOID HeapAlloc(HANDLE, DWORD flags, SIZE_T n) { return (flags & HEAP_ZERO_MEMORY) ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
LPVOID HeapReAlloc(HANDLE, DWORD flags, LPVOID p, SIZE_T n)
{
    size_t old = p ? malloc_size(p) : 0;
    void *r = realloc(p, n);
    if (r && (flags & HEAP_ZERO_MEMORY) && n > old)
        memset((char *)r + old, 0, n - old);
    return r;
}
BOOL HeapFree(HANDLE, DWORD, LPVOID p)
{
    free(p);
    return TRUE;
}
SIZE_T HeapSize(HANDLE, DWORD, LPCVOID p) { return malloc_size(p); }
BOOL HeapValidate(HANDLE, DWORD, LPCVOID) { return TRUE; }
HGLOBAL GlobalAlloc(UINT flags, SIZE_T n) { return (flags & GMEM_ZEROINIT) ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
HGLOBAL GlobalFree(HGLOBAL p)
{
    free(p);
    return nullptr;
}
LPVOID GlobalLock(HGLOBAL p) { return p; }
BOOL GlobalUnlock(HGLOBAL) { return TRUE; }
SIZE_T GlobalSize(HGLOBAL p) { return malloc_size(p); }
HLOCAL LocalAlloc(UINT flags, SIZE_T n) { return (flags & LMEM_ZEROINIT) ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
HLOCAL LocalFree(HLOCAL p)
{
    free(p);
    return nullptr;
}
void *_aligned_realloc(void *p, size_t size, size_t alignment)
{
    if (!p)
        return _aligned_malloc(size, alignment);
    size_t old = malloc_size(p);
    void *n = _aligned_malloc(size, alignment);
    if (n)
    {
        memcpy(n, p, old < size ? old : size);
        free(p);
    }
    return n;
}
size_t _msize(void *p) { return malloc_size(p); }

static void PhysicalMemory(unsigned long long *total, unsigned long long *avail)
{
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
    *total = mem;
    vm_statistics64_data_t vs;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &count) == KERN_SUCCESS)
        *avail = (unsigned long long)(vs.free_count + vs.inactive_count + vs.purgeable_count) * (unsigned long long)vm_page_size;
    else
        *avail = mem / 2;
}
void GlobalMemoryStatus(LPMEMORYSTATUS st)
{
    unsigned long long total, avail;
    PhysicalMemory(&total, &avail);
    st->dwLength = sizeof(*st);
    st->dwMemoryLoad = total ? (DWORD)(100 - avail * 100 / total) : 0;
    st->dwTotalPhys = (SIZE_T)total;
    st->dwAvailPhys = (SIZE_T)avail;
    st->dwTotalPageFile = (SIZE_T)total * 2;
    st->dwAvailPageFile = (SIZE_T)avail * 2;
    // the engine sizes itself as a 32-bit (large address aware) process
    st->dwTotalVirtual = 0xFFFE0000u;
    st->dwAvailVirtual = 0xC0000000u;
}
BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX st)
{
    unsigned long long total, avail;
    PhysicalMemory(&total, &avail);
    st->dwMemoryLoad = total ? (DWORD)(100 - avail * 100 / total) : 0;
    st->ullTotalPhys = total;
    st->ullAvailPhys = avail;
    st->ullTotalPageFile = total * 2;
    st->ullAvailPageFile = avail * 2;
    st->ullTotalVirtual = 0xFFFE0000ull;
    st->ullAvailVirtual = 0xC0000000ull;
    st->ullAvailExtendedVirtual = 0;
    return TRUE;
}
static DWORD CpuCount()
{
    int n = 0;
    size_t len = sizeof(n);
    if (sysctlbyname("hw.logicalcpu", &n, &len, nullptr, 0) != 0 || n <= 0)
        n = 4;
    return (DWORD)n;
}
void GetSystemInfo(LPSYSTEM_INFO si)
{
    memset(si, 0, sizeof(*si));
    si->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_INTEL; // the engine only knows the x86 answer
    si->dwPageSize = 4096;
    si->lpMinimumApplicationAddress = (LPVOID)0x10000;
    si->lpMaximumApplicationAddress = (LPVOID)0x7FFEFFFF;
    DWORD n = CpuCount();
    si->dwNumberOfProcessors = n;
    si->dwActiveProcessorMask = n >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << n) - 1);
    si->dwProcessorType = 586;
    si->dwAllocationGranularity = 65536;
    si->wProcessorLevel = 6;
}
void GetNativeSystemInfo(LPSYSTEM_INFO si) { GetSystemInfo(si); }
SIZE_T GetLargePageMinimum(void) { return 0; }
BOOL IsBadReadPtr(const void *p, UINT_PTR n) { return p == nullptr && n != 0; }
BOOL IsBadWritePtr(LPVOID p, UINT_PTR n) { return p == nullptr && n != 0; }
BOOL IsBadStringPtrA(LPCSTR p, UINT_PTR) { return p == nullptr; }
BOOL SetProcessWorkingSetSize(HANDLE, SIZE_T, SIZE_T) { return TRUE; }

// ---------------------------------------------------------------- time
BOOL QueryPerformanceCounter(LARGE_INTEGER *c)
{
    InitTime();
    c->QuadPart = (LONGLONG)mach_absolute_time();
    return TRUE;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f)
{
    InitTime();
    f->QuadPart = (LONGLONG)(1000000000ull * g_timebase.denom / g_timebase.numer);
    return TRUE;
}
DWORD GetTickCount(void) { return (DWORD)(MonotonicNs() / 1000000ull); }
ULONGLONG GetTickCount64(void) { return MonotonicNs() / 1000000ull; }
DWORD timeGetTime(void) { return (DWORD)(MonotonicNs() / 1000000ull); }
UINT timeBeginPeriod(UINT) { return 0; }
UINT timeEndPeriod(UINT) { return 0; }
void GetSystemTime(LPSYSTEMTIME st)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm;
    time_t t = tv.tv_sec;
    gmtime_r(&t, &tm);
    TmToSystemTime(tm, (int)(tv.tv_usec / 1000), st);
}
void GetLocalTime(LPSYSTEMTIME st)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm;
    time_t t = tv.tv_sec;
    localtime_r(&t, &tm);
    TmToSystemTime(tm, (int)(tv.tv_usec / 1000), st);
}
void GetSystemTimeAsFileTime(LPFILETIME ft)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    unsigned long long v = (unsigned long long)tv.tv_sec * 10000000ull + (unsigned long long)tv.tv_usec * 10ull + kFileTimeUnixEpoch;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}
BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min = st->wMinute;
    tm.tm_sec = st->wSecond;
    time_t t = timegm(&tm);
    unsigned long long v = (unsigned long long)t * 10000000ull + (unsigned long long)st->wMilliseconds * 10000ull + kFileTimeUnixEpoch;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
    return TRUE;
}
BOOL FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st)
{
    unsigned long long v = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (v < kFileTimeUnixEpoch)
        v = kFileTimeUnixEpoch;
    v -= kFileTimeUnixEpoch;
    time_t t = (time_t)(v / 10000000ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    TmToSystemTime(tm, (int)((v % 10000000ull) / 10000ull), st);
    return TRUE;
}
BOOL FileTimeToLocalFileTime(const FILETIME *in, LPFILETIME out)
{
    unsigned long long v = ((unsigned long long)in->dwHighDateTime << 32) | in->dwLowDateTime;
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    v += (long long)lt.tm_gmtoff * 10000000ll;
    out->dwLowDateTime = (DWORD)v;
    out->dwHighDateTime = (DWORD)(v >> 32);
    return TRUE;
}
LONG CompareFileTime(const FILETIME *a, const FILETIME *b)
{
    unsigned long long x = ((unsigned long long)a->dwHighDateTime << 32) | a->dwLowDateTime;
    unsigned long long y = ((unsigned long long)b->dwHighDateTime << 32) | b->dwLowDateTime;
    return x < y ? -1 : x > y ? 1 : 0;
}
DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz)
{
    memset(tz, 0, sizeof(*tz));
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    tz->Bias = (LONG)(-lt.tm_gmtoff / 60);
    return lt.tm_isdst > 0 ? TIME_ZONE_ID_DAYLIGHT : TIME_ZONE_ID_STANDARD;
}

// ---------------------------------------------------------------- processes / environment
HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
DWORD GetProcessId(HANDLE h) { return h == GetCurrentProcess() ? (DWORD)getpid() : 0; }
HANDLE OpenProcess(DWORD, BOOL, DWORD pid)
{
    if (pid == (DWORD)getpid())
        return GetCurrentProcess();
    if (kill((pid_t)pid, 0) != 0)
    {
        SetError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    KGeneric *p = new KGeneric(KType::Process);
    return (HANDLE)p;
}
void ExitProcess(UINT code)
{
    // Windows ends the process's other threads before anything else runs. exit() would run the static destructors
    // while the render, sound, streaming and shader threads still use those objects (a mutex locked after its
    // destructor ran throws, a condition variable destroyed under its waiters never returns with glibc).
    fflush(nullptr);
    _exit((int)code);
}
BOOL TerminateProcess(HANDLE h, UINT code)
{
    if (h == GetCurrentProcess())
    {
        fflush(nullptr);
        _exit((int)code);
    }
    return FALSE;
}
BOOL GetExitCodeProcess(HANDLE, LPDWORD code)
{
    if (code)
        *code = STILL_ACTIVE;
    return TRUE;
}
BOOL CreateProcessA(LPCSTR app, LPSTR cmdLine, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR dir, LPSTARTUPINFOA, LPPROCESS_INFORMATION pi)
{
    // Split the command line into argv (double quotes group words).
    std::vector<std::string> args;
    std::string cur;
    bool inq = false, any = false;
    const char *s = cmdLine ? cmdLine : app;
    for (; s && *s; ++s)
    {
        if (*s == '"')
        {
            inq = !inq;
            any = true;
        }
        else if (*s == ' ' && !inq)
        {
            if (any || !cur.empty())
                args.push_back(cur);
            cur.clear();
            any = false;
        }
        else
            cur += *s;
    }
    if (any || !cur.empty())
        args.push_back(cur);
    if (args.empty())
        return FALSE;
    std::string exe = app ? HostPath(app) : args[0];
    if (exe.size() > 4 && strcasecmp(exe.c_str() + exe.size() - 4, ".exe") == 0)
        exe = ExecutablePath(); // relaunching "BlackOps*.exe" means relaunching ourselves
    std::vector<char *> argv;
    argv.push_back((char *)exe.c_str());
    for (size_t i = 1; i < args.size(); ++i)
        argv.push_back((char *)args[i].c_str());
    argv.push_back(nullptr);
    std::string olddir;
    if (dir)
    {
        char buf[4096];
        if (getcwd(buf, sizeof(buf)))
            olddir = buf;
        chdir(HostPath(dir).c_str());
    }
    pid_t pid;
    int rc = posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ);
    if (!olddir.empty())
        chdir(olddir.c_str());
    if (rc != 0)
    {
        SetError(ErrnoToWin32(rc));
        return FALSE;
    }
    if (pi)
    {
        pi->hProcess = (HANDLE) new KGeneric(KType::Process);
        pi->hThread = (HANDLE) new KGeneric(KType::Other);
        pi->dwProcessId = (DWORD)pid;
        pi->dwThreadId = 0;
    }
    return TRUE;
}
BOOL SetPriorityClass(HANDLE, DWORD) { return TRUE; }
DWORD GetPriorityClass(HANDLE) { return NORMAL_PRIORITY_CLASS; }
BOOL GetProcessAffinityMask(HANDLE, PDWORD_PTR proc, PDWORD_PTR sys)
{
    DWORD n = CpuCount();
    DWORD_PTR m = n >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << n) - 1);
    if (proc)
        *proc = m;
    if (sys)
        *sys = m;
    return TRUE;
}
BOOL SetProcessAffinityMask(HANDLE, DWORD_PTR) { return TRUE; }
BOOL GetProcessTimes(HANDLE, LPFILETIME c, LPFILETIME e, LPFILETIME k, LPFILETIME u)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    unsigned long long uu = (unsigned long long)ru.ru_utime.tv_sec * 10000000ull + ru.ru_utime.tv_usec * 10ull;
    unsigned long long kk = (unsigned long long)ru.ru_stime.tv_sec * 10000000ull + ru.ru_stime.tv_usec * 10ull;
    if (c) GetSystemTimeAsFileTime(c);
    if (e) e->dwLowDateTime = e->dwHighDateTime = 0;
    if (k) { k->dwLowDateTime = (DWORD)kk; k->dwHighDateTime = (DWORD)(kk >> 32); }
    if (u) { u->dwLowDateTime = (DWORD)uu; u->dwHighDateTime = (DWORD)(uu >> 32); }
    return TRUE;
}
BOOL QueryProcessCycleTime(HANDLE, PULONG64 cycles)
{
    if (cycles)
        *cycles = clock_gettime_nsec_np(CLOCK_PROCESS_CPUTIME_ID);
    return TRUE;
}
BOOL GetSystemTimes(LPFILETIME idle, LPFILETIME kernel, LPFILETIME user)
{
    host_cpu_load_info_data_t load;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&load, &count) != KERN_SUCCESS)
        return FALSE;
    auto put = [](LPFILETIME ft, unsigned long long ticks) {
        if (!ft)
            return;
        unsigned long long v = ticks * 100000ull; // 10 ms ticks -> 100 ns
        ft->dwLowDateTime = (DWORD)v;
        ft->dwHighDateTime = (DWORD)(v >> 32);
    };
    put(idle, load.cpu_ticks[CPU_STATE_IDLE]);
    put(kernel, load.cpu_ticks[CPU_STATE_SYSTEM] + load.cpu_ticks[CPU_STATE_IDLE]);
    put(user, load.cpu_ticks[CPU_STATE_USER] + load.cpu_ticks[CPU_STATE_NICE]);
    return TRUE;
}
BOOL ReadProcessMemory(HANDLE h, LPCVOID addr, LPVOID buf, SIZE_T n, SIZE_T *got)
{
    if (h != GetCurrentProcess())
        return FALSE;
    mach_vm_size_t out = 0;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)addr, n, (mach_vm_address_t)buf, &out);
    if (got)
        *got = (SIZE_T)out;
    return kr == KERN_SUCCESS;
}
BOOL WriteProcessMemory(HANDLE h, LPVOID addr, LPCVOID buf, SIZE_T n, SIZE_T *wrote)
{
    if (h != GetCurrentProcess())
        return FALSE;
    memcpy(addr, buf, n);
    if (wrote)
        *wrote = n;
    return TRUE;
}
BOOL FlushInstructionCache(HANDLE, LPCVOID p, SIZE_T n)
{
    __builtin___clear_cache((char *)p, (char *)p + n);
    return TRUE;
}
UINT SetErrorMode(UINT m) { return m; }
LPSTR GetCommandLineA(void) { return (LPSTR)g_commandLine.c_str(); }
LPWSTR GetCommandLineW(void)
{
    static std::wstring w;
    w.assign(g_commandLine.begin(), g_commandLine.end());
    return (LPWSTR)w.c_str();
}
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD n)
{
    const char *v = getenv(name);
    if (!v)
    {
        SetError(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    DWORD len = (DWORD)strlen(v);
    if (!buf || n <= len)
        return len + 1;
    memcpy(buf, v, len + 1);
    return len;
}
BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value)
{
    if (value)
        return setenv(name, value, 1) == 0;
    return unsetenv(name) == 0;
}
DWORD ExpandEnvironmentStringsA(LPCSTR src, LPSTR dst, DWORD n)
{
    std::string out;
    for (const char *s = src; *s; ++s)
    {
        if (*s == '%')
        {
            const char *e = strchr(s + 1, '%');
            if (e)
            {
                std::string name(s + 1, e - s - 1);
                const char *v = getenv(name.c_str());
                out += v ? v : std::string("%") + name + "%";
                s = e;
                continue;
            }
        }
        out += *s;
    }
    if (dst && n > out.size())
        memcpy(dst, out.c_str(), out.size() + 1);
    return (DWORD)out.size() + 1;
}
BOOL IsDebuggerPresent(void)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info;
    size_t size = sizeof(info);
    memset(&info, 0, sizeof(info));
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0)
        return FALSE;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
}
void DebugBreak(void)
{
    if (IsDebuggerPresent())
        __builtin_debugtrap();
}
void OutputDebugStringA(LPCSTR s)
{
    if (s)
        fputs(s, stderr);
}
void OutputDebugStringW(LPCWSTR s)
{
    if (s)
        fprintf(stderr, "%ls", s);
}
void RaiseException(DWORD code, DWORD, DWORD nargs, const ULONG_PTR *args)
{
    if (code == 0x406D1388 && nargs >= 2 && args)
    {
        // MSVC "set thread name" exception. The engine's THREADNAME_INFO (qcommon/threads.h) keeps the 32-bit
        // layout and passes its size in DWORDs: { DWORD type = 0x1000, 32-bit name pointer, DWORD threadId,
        // DWORD flags }.
        const uint32_t *w = (const uint32_t *)args;
        if (w[0] == 0x1000)
        {
            const char *name = (const char *)Ptr32_Decode(w[1]);
            const DWORD tid = nargs >= 3 ? (DWORD)w[2] : (DWORD)-1;
            if (name && (tid == (DWORD)-1 || tid == GetCurrentThreadId()))
                pthread_setname_np(name);
        }
        return;
    }
    fprintf(stderr, "RaiseException(0x%08X)\n", code);
    if (g_unhandledFilter)
    {
        EXCEPTION_RECORD rec;
        memset(&rec, 0, sizeof(rec));
        rec.ExceptionCode = code;
        CONTEXT ctx;
        RtlCaptureContext(&ctx);
        EXCEPTION_POINTERS ep = {&rec, &ctx};
        g_unhandledFilter(&ep);
    }
    abort();
}
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    LPTOP_LEVEL_EXCEPTION_FILTER old = g_unhandledFilter;
    g_unhandledFilter = f;
    InstallCrashHandlers();
    return old;
}
LONG UnhandledExceptionFilter(struct _EXCEPTION_POINTERS *) { return EXCEPTION_CONTINUE_SEARCH; }
PVOID AddVectoredExceptionHandler(ULONG, PVECTORED_EXCEPTION_HANDLER h) { return (PVOID)h; }
ULONG RemoveVectoredExceptionHandler(PVOID) { return 1; }
BOOL GetVersionExA(LPOSVERSIONINFOA v)
{
    // Report a current Windows version so version gates in the engine take their modern paths.
    DWORD size = v->dwOSVersionInfoSize;
    memset((char *)v + sizeof(DWORD), 0, size - sizeof(DWORD));
    v->dwMajorVersion = 10;
    v->dwMinorVersion = 0;
    v->dwBuildNumber = 19045;
    v->dwPlatformId = VER_PLATFORM_WIN32_NT;
    return TRUE;
}
DWORD GetVersion(void) { return (19045u << 16) | (0u << 8) | 10u; }
BOOL IsWow64Process(HANDLE, PBOOL w)
{
    if (w)
        *w = FALSE;
    return TRUE;
}
BOOL IsProcessorFeaturePresent(DWORD f)
{
    // SSE/SSE2/SSE3 code paths run through sse2neon
    return f == PF_XMMI_INSTRUCTIONS_AVAILABLE || f == PF_XMMI64_INSTRUCTIONS_AVAILABLE || f == PF_SSE3_INSTRUCTIONS_AVAILABLE;
}
BOOL GetComputerNameA(LPSTR buf, LPDWORD n)
{
    char host[256];
    if (gethostname(host, sizeof(host)) != 0)
        strcpy(host, "mac");
    char *dot = strchr(host, '.');
    if (dot)
        *dot = 0;
    DWORD len = (DWORD)strlen(host);
    if (!buf || *n <= len)
    {
        *n = len + 1;
        SetError(ERROR_BUFFER_OVERFLOW);
        return FALSE;
    }
    memcpy(buf, host, len + 1);
    *n = len;
    return TRUE;
}
BOOL GetUserNameA(LPSTR buf, LPDWORD n)
{
    const char *u = getenv("USER");
    if (!u)
    {
        struct passwd *pw = getpwuid(getuid());
        u = pw ? pw->pw_name : "player";
    }
    DWORD len = (DWORD)strlen(u);
    if (!buf || *n <= len)
    {
        *n = len + 1;
        SetError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(buf, u, len + 1);
    *n = len + 1;
    return TRUE;
}
BOOL OpenThreadToken(HANDLE, DWORD, BOOL, PHANDLE t)
{
    // Threads never impersonate here: callers fall back to the process token, as on Windows.
    *t = nullptr;
    SetError(ERROR_NO_TOKEN);
    return FALSE;
}
BOOL OpenProcessToken(HANDLE, DWORD, PHANDLE t)
{
    *t = (HANDLE) new KGeneric(KType::Token);
    return TRUE;
}
BOOL LookupPrivilegeValueA(LPCSTR, LPCSTR, PLUID luid)
{
    luid->LowPart = 1;
    luid->HighPart = 0;
    return TRUE;
}
BOOL AdjustTokenPrivileges(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES, PDWORD)
{
    SetError(ERROR_SUCCESS);
    return TRUE;
}
BOOL GetTokenInformation(HANDLE, TOKEN_INFORMATION_CLASS cls, LPVOID info, DWORD len, PDWORD ret)
{
    if (cls == TokenElevationType && info && len >= sizeof(TOKEN_ELEVATION_TYPE))
    {
        *(TOKEN_ELEVATION_TYPE *)info = TokenElevationTypeDefault;
        if (ret)
            *ret = sizeof(TOKEN_ELEVATION_TYPE);
        return TRUE;
    }
    if (cls == TokenElevation && info && len >= sizeof(TOKEN_ELEVATION))
    {
        ((TOKEN_ELEVATION *)info)->TokenIsElevated = 0;
        if (ret)
            *ret = sizeof(TOKEN_ELEVATION);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- modules
// The executable itself is the only "module"; system DLL lookups fail and callers take their fallbacks.
static int g_mainModuleTag;
HMODULE GetModuleHandleA(LPCSTR name)
{
    if (!name)
        return (HMODULE)&g_mainModuleTag;
    SetError(ERROR_MOD_NOT_FOUND);
    return nullptr;
}
HMODULE GetModuleHandleW(LPCWSTR name) { return name ? nullptr : (HMODULE)&g_mainModuleTag; }
BOOL GetModuleHandleExA(DWORD, LPCSTR, HMODULE *m)
{
    if (m)
        *m = (HMODULE)&g_mainModuleTag;
    return TRUE;
}
DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD n)
{
    std::string p = g_gameDir.empty() ? ExecutablePath() : g_gameDir + "/BO1Zombies";
    if (!buf || !n)
        return 0;
    size_t len = p.size() < n - 1 ? p.size() : n - 1;
    memcpy(buf, p.c_str(), len);
    buf[len] = 0;
    if (len < p.size())
        SetError(ERROR_INSUFFICIENT_BUFFER);
    return (DWORD)len;
}
DWORD GetModuleFileNameW(HMODULE m, LPWSTR buf, DWORD n)
{
    char tmp[4096];
    DWORD len = GetModuleFileNameA(m, tmp, sizeof(tmp));
    return (DWORD)MultiByteToWideChar(CP_UTF8, 0, tmp, (int)len + 1, buf, (int)n) - 1;
}
HMODULE LoadLibraryA(LPCSTR name)
{
    (void)name;
    SetError(ERROR_MOD_NOT_FOUND);
    return nullptr;
}
HMODULE LoadLibraryW(LPCWSTR) { SetError(ERROR_MOD_NOT_FOUND); return nullptr; }
HMODULE LoadLibraryExA(LPCSTR name, HANDLE, DWORD) { return LoadLibraryA(name); }
BOOL FreeLibrary(HMODULE) { return TRUE; }
FARPROC GetProcAddress(HMODULE, LPCSTR)
{
    SetError(ERROR_PROC_NOT_FOUND);
    return nullptr;
}

// ---------------------------------------------------------------- strings / locale
int MultiByteToWideChar(UINT cp, DWORD, LPCSTR src, int srcLen, LPWSTR dst, int dstLen)
{
    if (!src)
        return 0;
    if (srcLen < 0)
        srcLen = (int)strlen(src) + 1;
    int out = 0;
    for (int i = 0; i < srcLen;)
    {
        unsigned c = (unsigned char)src[i];
        unsigned cpv;
        int len = 1;
        if (cp == CP_UTF8 && c >= 0x80)
        {
            if ((c & 0xE0) == 0xC0 && i + 1 < srcLen) { cpv = ((c & 0x1F) << 6) | (src[i + 1] & 0x3F); len = 2; }
            else if ((c & 0xF0) == 0xE0 && i + 2 < srcLen) { cpv = ((c & 0x0F) << 12) | ((src[i + 1] & 0x3F) << 6) | (src[i + 2] & 0x3F); len = 3; }
            else if ((c & 0xF8) == 0xF0 && i + 3 < srcLen) { cpv = ((c & 0x07) << 18) | ((src[i + 1] & 0x3F) << 12) | ((src[i + 2] & 0x3F) << 6) | (src[i + 3] & 0x3F); len = 4; }
            else cpv = 0xFFFD;
        }
        else
            cpv = c; // CP_ACP: Latin-1
        if (dst && dstLen)
        {
            if (out >= dstLen)
            {
                SetError(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            dst[out] = (WCHAR)cpv;
        }
        ++out;
        i += len;
    }
    return out;
}
int WideCharToMultiByte(UINT cp, DWORD, LPCWSTR src, int srcLen, LPSTR dst, int dstLen, LPCSTR defChar, LPBOOL usedDef)
{
    if (!src)
        return 0;
    if (srcLen < 0)
        srcLen = (int)wcslen(src) + 1;
    if (usedDef)
        *usedDef = FALSE;
    int out = 0;
    for (int i = 0; i < srcLen; ++i)
    {
        unsigned c = (unsigned)src[i];
        char buf[4];
        int len;
        if (cp == CP_UTF8)
        {
            if (c < 0x80) { buf[0] = (char)c; len = 1; }
            else if (c < 0x800) { buf[0] = (char)(0xC0 | (c >> 6)); buf[1] = (char)(0x80 | (c & 0x3F)); len = 2; }
            else if (c < 0x10000) { buf[0] = (char)(0xE0 | (c >> 12)); buf[1] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[2] = (char)(0x80 | (c & 0x3F)); len = 3; }
            else { buf[0] = (char)(0xF0 | (c >> 18)); buf[1] = (char)(0x80 | ((c >> 12) & 0x3F)); buf[2] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[3] = (char)(0x80 | (c & 0x3F)); len = 4; }
        }
        else
        {
            if (c < 256)
                buf[0] = (char)c;
            else
            {
                buf[0] = defChar ? *defChar : '?';
                if (usedDef)
                    *usedDef = TRUE;
            }
            len = 1;
        }
        if (dst && dstLen)
        {
            if (out + len > dstLen)
            {
                SetError(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            memcpy(dst + out, buf, len);
        }
        out += len;
    }
    return out;
}
LCID GetUserDefaultLCID(void) { return 0x0409; }
LANGID GetUserDefaultLangID(void) { return 0x0409; }
LANGID GetUserDefaultUILanguage(void) { return 0x0409; }
LANGID GetSystemDefaultLangID(void) { return 0x0409; }
UINT GetACP(void) { return 1252; }
int GetLocaleInfoA(LCID, DWORD type, LPSTR buf, int n)
{
    const char *v = "";
    switch (type)
    {
    case LOCALE_SISO639LANGNAME: v = "en"; break;
    case LOCALE_SISO3166CTRYNAME: v = "US"; break;
    case LOCALE_SENGLANGUAGE: v = "English"; break;
    default: break;
    }
    int len = (int)strlen(v) + 1;
    if (buf && n >= len)
        memcpy(buf, v, len);
    return len;
}
LPSTR CharUpperA(LPSTR s)
{
    if ((uintptr_t)s < 0x10000)
        return (LPSTR)(uintptr_t)toupper((int)(uintptr_t)s);
    return _strupr(s);
}
LPSTR CharLowerA(LPSTR s)
{
    if ((uintptr_t)s < 0x10000)
        return (LPSTR)(uintptr_t)tolower((int)(uintptr_t)s);
    return _strlwr(s);
}
DWORD FormatMessageA(DWORD flags, LPCVOID, DWORD id, DWORD, LPSTR buf, DWORD n, va_list *)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "Win32 error %u", id);
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER)
    {
        *(char **)buf = strdup(tmp);
        return (DWORD)strlen(tmp);
    }
    if (!buf || !n)
        return 0;
    strncpy(buf, tmp, n - 1);
    buf[n - 1] = 0;
    return (DWORD)strlen(buf);
}

// ---------------------------------------------------------------- registry: never has anything
LSTATUS RegOpenKeyExA(HKEY, LPCSTR, DWORD, REGSAM, PHKEY r) { if (r) *r = nullptr; return ERROR_FILE_NOT_FOUND; }
LSTATUS RegOpenKeyA(HKEY, LPCSTR, PHKEY r) { if (r) *r = nullptr; return ERROR_FILE_NOT_FOUND; }
LSTATUS RegCreateKeyExA(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY r, LPDWORD) { if (r) *r = nullptr; return ERROR_ACCESS_DENIED; }
LSTATUS RegQueryValueExA(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
LSTATUS RegSetValueExA(HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD) { return ERROR_ACCESS_DENIED; }
LSTATUS RegDeleteValueA(HKEY, LPCSTR) { return ERROR_FILE_NOT_FOUND; }
LSTATUS RegDeleteKeyA(HKEY, LPCSTR) { return ERROR_FILE_NOT_FOUND; }
LSTATUS RegCloseKey(HKEY) { return ERROR_SUCCESS; }
LSTATUS RegGetValueA(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD) { return ERROR_FILE_NOT_FOUND; }

// ---------------------------------------------------------------- psapi / toolhelp
BOOL GetProcessMemoryInfo(HANDLE, PPROCESS_MEMORY_COUNTERS c, DWORD cb)
{
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
        return FALSE;
    memset(c, 0, cb);
    c->cb = cb;
    c->WorkingSetSize = (SIZE_T)info.resident_size;
    c->PeakWorkingSetSize = (SIZE_T)info.resident_size_max;
    c->PagefileUsage = (SIZE_T)info.resident_size;
    c->PeakPagefileUsage = (SIZE_T)info.resident_size_max;
    if (cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX))
        ((PPROCESS_MEMORY_COUNTERS_EX)c)->PrivateUsage = (SIZE_T)info.resident_size;
    return TRUE;
}
BOOL K32GetProcessMemoryInfo(HANDLE h, PPROCESS_MEMORY_COUNTERS c, DWORD cb) { return GetProcessMemoryInfo(h, c, cb); }
BOOL GetModuleInformation(HANDLE, HMODULE, LPMODULEINFO mi, DWORD)
{
    const struct mach_header *mh = _dyld_get_image_header(0);
    mi->lpBaseOfDll = (LPVOID)mh;
    mi->SizeOfImage = 0x10000000;
    mi->EntryPoint = nullptr;
    return TRUE;
}
BOOL EnumProcessModules(HANDLE, HMODULE *mods, DWORD cb, LPDWORD needed)
{
    if (needed)
        *needed = sizeof(HMODULE);
    if (mods && cb >= sizeof(HMODULE))
        mods[0] = (HMODULE)&g_mainModuleTag;
    return TRUE;
}
DWORD GetModuleBaseNameA(HANDLE, HMODULE, LPSTR buf, DWORD n)
{
    std::string p = ExecutablePath();
    const char *b = strrchr(p.c_str(), '/');
    b = b ? b + 1 : p.c_str();
    strncpy(buf, b, n - 1);
    buf[n - 1] = 0;
    return (DWORD)strlen(buf);
}
DWORD GetModuleFileNameExA(HANDLE, HMODULE m, LPSTR buf, DWORD n) { return GetModuleFileNameA(m, buf, n); }
DWORD GetProcessImageFileNameA(HANDLE, LPSTR buf, DWORD n) { return GetModuleFileNameA(nullptr, buf, n); }
BOOL QueryFullProcessImageNameA(HANDLE h, DWORD, LPSTR buf, PDWORD n)
{
    if (h != GetCurrentProcess())
        return FALSE;
    *n = GetModuleFileNameA(nullptr, buf, *n);
    return TRUE;
}
HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD)
{
    KSnapshot *s = new KSnapshot();
    if (flags & TH32CS_SNAPTHREAD)
        ForEachThread([](KThread *t, void *ctx) { if (!t->finished) ((KSnapshot *)ctx)->threadIds.push_back(t->id); }, s);
    return (HANDLE)s;
}
BOOL Thread32First(HANDLE h, LPTHREADENTRY32 te)
{
    KSnapshot *s = (KSnapshot *)h;
    s->next = 0;
    return Thread32Next(h, te);
}
BOOL Thread32Next(HANDLE h, LPTHREADENTRY32 te)
{
    KSnapshot *s = (KSnapshot *)h;
    if (s->next >= s->threadIds.size())
    {
        SetError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    te->th32ThreadID = s->threadIds[s->next++];
    te->th32OwnerProcessID = (DWORD)getpid();
    te->tpBasePri = 8;
    te->tpDeltaPri = 0;
    return TRUE;
}
BOOL Process32First(HANDLE, LPPROCESSENTRY32) { return FALSE; }
BOOL Process32Next(HANDLE, LPPROCESSENTRY32) { return FALSE; }
BOOL Module32First(HANDLE, LPMODULEENTRY32) { return FALSE; }
BOOL Module32Next(HANDLE, LPMODULEENTRY32) { return FALSE; }

// ---------------------------------------------------------------- dbghelp (no PDB symbols on macOS)
BOOL SymInitialize(HANDLE, PCSTR, BOOL) { return TRUE; }
BOOL SymCleanup(HANDLE) { return TRUE; }
DWORD SymSetOptions(DWORD o) { return o; }
DWORD SymGetOptions(void) { return 0; }
BOOL SymFromAddr(HANDLE, DWORD64 addr, PDWORD64 disp, PSYMBOL_INFO sym)
{
    Dl_info info;
    if (!dladdr((void *)addr, &info) || !info.dli_sname)
        return FALSE;
    if (disp)
        *disp = addr - (DWORD64)info.dli_saddr;
    strncpy(sym->Name, info.dli_sname, sym->MaxNameLen ? sym->MaxNameLen - 1 : 0);
    sym->Address = (DWORD64)info.dli_saddr;
    return TRUE;
}
BOOL SymGetSymFromAddr64(HANDLE, DWORD64 addr, PDWORD64 disp, PIMAGEHLP_SYMBOL64 sym)
{
    Dl_info info;
    if (!dladdr((void *)addr, &info) || !info.dli_sname)
        return FALSE;
    if (disp)
        *disp = addr - (DWORD64)info.dli_saddr;
    strncpy(sym->Name, info.dli_sname, sym->MaxNameLength ? sym->MaxNameLength - 1 : 0);
    sym->Address = (DWORD64)info.dli_saddr;
    return TRUE;
}
BOOL SymGetLineFromAddr64(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64) { return FALSE; }
BOOL SymGetModuleInfo64(HANDLE, DWORD64, PIMAGEHLP_MODULE64) { return FALSE; }
DWORD64 SymLoadModule64(HANDLE, HANDLE, PCSTR, PCSTR, DWORD64, DWORD) { return 0; }
PVOID SymFunctionTableAccess64(HANDLE, DWORD64) { return nullptr; }
DWORD64 SymGetModuleBase64(HANDLE, DWORD64) { return 0; }
BOOL StackWalk64(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64) { return FALSE; }
BOOL MiniDumpWriteDump(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION) { return FALSE; }
DWORD UnDecorateSymbolName(PCSTR name, PSTR out, DWORD n, DWORD)
{
    strncpy(out, name, n - 1);
    out[n - 1] = 0;
    return (DWORD)strlen(out);
}
WORD RtlCaptureStackBackTrace(DWORD skip, DWORD count, PVOID *frames, PDWORD hash)
{
    void *buf[128];
    int n = backtrace(buf, 128);
    int got = 0;
    for (int i = (int)skip + 1; i < n && got < (int)count; ++i)
        frames[got++] = buf[i];
    if (hash)
    {
        DWORD h = 0;
        for (int i = 0; i < got; ++i)
            h = h * 31 + (DWORD)(uintptr_t)frames[i];
        *hash = h;
    }
    return (WORD)got;
}
void RtlCaptureContext(PCONTEXT ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->Pc = (DWORD64)__builtin_return_address(0);
    ctx->Fp = (DWORD64)__builtin_frame_address(0);
    uint64_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    ctx->Sp = sp;
    ctx->Eip = (DWORD_PTR)ctx->Pc;
    ctx->Esp = (DWORD_PTR)ctx->Sp;
    ctx->Ebp = (DWORD_PTR)ctx->Fp;
    ctx->ContextFlags = CONTEXT_CONTROL;
}

// ---------------------------------------------------------------- shell folders
HRESULT SHGetFolderPathA(HWND, int csidl, HANDLE, DWORD, LPSTR path)
{
    const char *home = getenv("HOME");
    if (!home)
    {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    std::string p;
    switch (csidl & 0xFF)
    {
    case CSIDL_PERSONAL: p = std::string(home) + "/Documents"; break;
    case CSIDL_DESKTOP: p = std::string(home) + "/Desktop"; break;
    case CSIDL_APPDATA:
    case CSIDL_LOCAL_APPDATA:
    case CSIDL_COMMON_APPDATA: p = std::string(home) + "/Library/Application Support"; break;
    default: p = home; break;
    }
    if (csidl & CSIDL_FLAG_CREATE)
        mkdir(p.c_str(), 0755);
    strncpy(path, p.c_str(), MAX_PATH - 1);
    path[MAX_PATH - 1] = 0;
    return S_OK;
}
BOOL SHGetSpecialFolderPathA(HWND w, LPSTR path, int csidl, BOOL create)
{
    return SUCCEEDED(SHGetFolderPathA(w, csidl | (create ? CSIDL_FLAG_CREATE : 0), nullptr, 0, path));
}
int SHCreateDirectoryExA(HWND, LPCSTR path, const SECURITY_ATTRIBUTES *)
{
    std::string p = HostPath(path);
    for (size_t i = 1; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '/')
            mkdir(p.substr(0, i).c_str(), 0755);
    return ERROR_SUCCESS;
}

// ---------------------------------------------------------------- COM (nothing registered)
HRESULT CoInitialize(LPVOID) { return S_OK; }
HRESULT CoInitializeEx(LPVOID, DWORD) { return S_OK; }
void CoUninitialize(void) {}
HRESULT CoCreateInstance(REFCLSID, void *, DWORD, REFIID, LPVOID *ppv)
{
    if (ppv)
        *ppv = nullptr;
    return E_NOINTERFACE;
}
HRESULT CLSIDFromString(LPCWSTR s, LPCLSID out)
{
    if (!s || !out)
        return E_INVALIDARG;
    unsigned d1, d2, d3, b[8];
    char buf[64];
    WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf), nullptr, nullptr);
    if (sscanf(buf, "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return E_INVALIDARG;
    out->Data1 = d1;
    out->Data2 = (uint16_t)d2;
    out->Data3 = (uint16_t)d3;
    for (int i = 0; i < 8; ++i)
        out->Data4[i] = (uint8_t)b[i];
    return S_OK;
}
int StringFromGUID2(REFGUID g, LPWSTR out, int n)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return MultiByteToWideChar(CP_UTF8, 0, buf, -1, out, n);
}
void CoTaskMemFree(LPVOID p) { free(p); }

// ---------------------------------------------------------------- multimedia mixer: no capture devices
UINT mixerGetNumDevs(void) { return 0; }
MMRESULT mixerOpen(LPHMIXER h, UINT, DWORD_PTR, DWORD_PTR, DWORD) { if (h) *h = nullptr; return MMSYSERR_NODRIVER; }
MMRESULT mixerClose(HMIXER) { return MMSYSERR_NOERROR; }
MMRESULT mixerGetID(HMIXEROBJ, UINT *id, DWORD) { if (id) *id = 0; return MMSYSERR_NODRIVER; }
MMRESULT mixerGetDevCapsA(UINT_PTR, LPMIXERCAPSA, UINT) { return MMSYSERR_BADDEVICEID; }
MMRESULT mixerGetLineInfoA(HMIXEROBJ, LPMIXERLINEA, DWORD) { return MMSYSERR_NODRIVER; }
MMRESULT mixerGetLineControlsA(HMIXEROBJ, LPMIXERLINECONTROLSA, DWORD) { return MMSYSERR_NODRIVER; }
MMRESULT mixerGetControlDetailsA(HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD) { return MMSYSERR_NODRIVER; }
MMRESULT mixerSetControlDetails(HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD) { return MMSYSERR_NODRIVER; }
UINT waveInGetNumDevs(void) { return 0; }
MMRESULT waveInGetDevCapsA(UINT_PTR, LPWAVEINCAPSA, UINT) { return MMSYSERR_BADDEVICEID; }

// ---------------------------------------------------------------- CPU topology
BOOL GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG ret, HANDLE, ULONG)
{
    if (ret)
        *ret = 0;
    SetError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
BOOL SetThreadSelectedCpuSets(HANDLE, const ULONG *, ULONG) { return TRUE; }
BOOL GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buf, PDWORD len)
{
    int phys = 0;
    size_t l = sizeof(phys);
    if (sysctlbyname("hw.physicalcpu", &phys, &l, nullptr, 0) != 0 || phys <= 0)
        phys = (int)CpuCount();
    DWORD need = (DWORD)(phys * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!buf || *len < need)
    {
        *len = need;
        SetError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(buf, 0, need);
    for (int i = 0; i < phys; ++i)
    {
        buf[i].Relationship = RelationProcessorCore;
        buf[i].ProcessorMask = (ULONG_PTR)1 << i;
    }
    *len = need;
    return TRUE;
}

// ---------------------------------------------------------------- assorted
EXECUTION_STATE SetThreadExecutionState(EXECUTION_STATE s) { return s; }
int MulDiv(int a, int b, int c)
{
    if (!c)
        return -1;
    long long r = (long long)a * b;
    r = (r >= 0) ? (r + c / 2) / c : (r - c / 2) / c;
    return (int)r;
}
BOOL GetProcessHandleCount(HANDLE, PDWORD n)
{
    if (n)
        *n = 0;
    return TRUE;
}
BOOL EqualRect(const RECT *a, const RECT *b) { return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom; }
BOOL SetRect(LPRECT r, int l, int t, int rr, int b)
{
    r->left = l;
    r->top = t;
    r->right = rr;
    r->bottom = b;
    return TRUE;
}
BOOL IntersectRect(LPRECT d, const RECT *a, const RECT *b)
{
    d->left = a->left > b->left ? a->left : b->left;
    d->top = a->top > b->top ? a->top : b->top;
    d->right = a->right < b->right ? a->right : b->right;
    d->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (d->left >= d->right || d->top >= d->bottom)
    {
        memset(d, 0, sizeof(*d));
        return FALSE;
    }
    return TRUE;
}
BOOL OffsetRect(LPRECT r, int dx, int dy)
{
    r->left += dx;
    r->right += dx;
    r->top += dy;
    r->bottom += dy;
    return TRUE;
}
BOOL PtInRect(const RECT *r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
HFONT CreateFontA(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR) { return nullptr; }

} // extern "C"

namespace w32
{
// Called once from main() before anything else runs.
void PlatformInit(int argc, char **argv)
{
    InitTime();
    SetCommandLine(argc, argv);
    bo1ptr::InitArena();
    {
        // the main thread's stack is not in the arena: give its locals 32-bit handles through a window
        // (the reported size can be the rlimit rather than the linked -stack_size; cover the larger of the two)
        char *top = (char *)pthread_get_stackaddr_np(pthread_self());
        size_t size = pthread_get_stacksize_np(pthread_self());
        if (size < ((size_t)64 << 20))
            size = (size_t)64 << 20;
        if (top)
            bo1ptr::RegisterWindow(top - size, size);
    }
    CurrentThread(); // the main thread gets its id first
    // AppKit work requested by other threads runs here, woken from the Win32 waits (main_thread.h)
    SetMainThread();
    AddMainThreadWaker(WakeAllWaiters);
    InstallCrashHandlers();
}
} // namespace w32

// MSVC CRT rand()/srand() (msvc_crt.h maps the engine's calls here): the CRT's linear congruential generator, its
// state per thread and starting at 1 in every thread, as the multithreaded CRT keeps _holdrand.
static thread_local unsigned int t_msvcHoldRand = 1;

extern "C" int bo1_msvc_rand(void)
{
    t_msvcHoldRand = t_msvcHoldRand * 214013u + 2531011u;
    return (int)((t_msvcHoldRand >> 16) & 0x7FFFu);
}

extern "C" void bo1_msvc_srand(unsigned int seed)
{
    t_msvcHoldRand = seed;
}
