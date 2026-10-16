// Win32 kernel objects on POSIX/Mach for the macOS port: handles, events, mutexes, semaphores, waits, threads,
// thread-local storage, critical sections, SRW locks, condition variables and user-mode APCs.

#include <windows.h>
#include "kernel32_internal.h"
#include "arena.h"
#include "main_thread.h"

#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>
#include <time.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace w32
{
std::mutex g_waitLock;
std::condition_variable g_waitCv;

static thread_local DWORD t_lastError;
static thread_local KThread *t_self;
static std::atomic<DWORD> g_nextThreadId{0x100};
static std::mutex g_threadTableLock;
static std::unordered_map<DWORD, KThread *> g_threadTable;

KObject::~KObject() {}

void KObject::Ref() { refs.fetch_add(1, std::memory_order_relaxed); }

void KObject::Unref()
{
    if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete this;
}

DWORD ThreadSelfId()
{
    return CurrentThread()->id;
}

KThread *CurrentThread()
{
    if (!t_self)
    {
        // A thread not created through CreateThread (the main thread, or one created by a system library).
        KThread *t = new KThread();
        t->pthread = pthread_self();
        t->machThread = pthread_mach_thread_np(pthread_self());
        t->id = g_nextThreadId.fetch_add(4);
        t->started = true;
        t->adopted = true;
        t_self = t;
        std::lock_guard<std::mutex> g(g_threadTableLock);
        g_threadTable[t->id] = t;
    }
    return t_self;
}

KThread *FindThread(DWORD id)
{
    std::lock_guard<std::mutex> g(g_threadTableLock);
    auto it = g_threadTable.find(id);
    return it == g_threadTable.end() ? nullptr : it->second;
}

void ForEachThread(void (*fn)(KThread *, void *), void *ctx)
{
    std::lock_guard<std::mutex> g(g_threadTableLock);
    for (auto &kv : g_threadTable)
        fn(kv.second, ctx);
}

void SetError(DWORD e) { t_lastError = e; }

DWORD ErrnoToWin32(int e)
{
    switch (e)
    {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case EMFILE:
    case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case ENOSPC: return ERROR_DISK_FULL;
    case EBADF: return ERROR_INVALID_HANDLE;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case ENAMETOOLONG: return ERROR_INVALID_NAME;
    case EROFS: return ERROR_WRITE_PROTECT;
    case ETIMEDOUT: return ERROR_TIMEOUT;
    default: return ERROR_INVALID_FUNCTION;
    }
}

// -------- waits --------

static bool IsSignaled(KObject *o, KThread *self)
{
    switch (o->type)
    {
    case KType::Event: return static_cast<KEvent *>(o)->signaled;
    case KType::Mutex:
    {
        KMutex *m = static_cast<KMutex *>(o);
        return m->owner == 0 || m->owner == self->id;
    }
    case KType::Semaphore: return static_cast<KSemaphore *>(o)->count > 0;
    case KType::Thread: return static_cast<KThread *>(o)->finished;
    case KType::Process: return false;
    default: return true; // files etc. are always "signaled"
    }
}

static void Consume(KObject *o, KThread *self)
{
    switch (o->type)
    {
    case KType::Event:
    {
        KEvent *e = static_cast<KEvent *>(o);
        if (!e->manualReset)
            e->signaled = false;
        break;
    }
    case KType::Mutex:
    {
        KMutex *m = static_cast<KMutex *>(o);
        m->owner = self->id;
        m->recursion++;
        break;
    }
    case KType::Semaphore: static_cast<KSemaphore *>(o)->count--; break;
    default: break;
    }
}

static KObject *Resolve(HANDLE h)
{
    if (h == GetCurrentThread())
        return CurrentThread();
    if (!h || h == INVALID_HANDLE_VALUE || h == GetCurrentProcess())
        return nullptr;
    return (KObject *)h;
}

// Runs this thread's queued APCs (completion routines). Returns true if any ran.
static bool RunApcs(KThread *self)
{
    bool ran = false;
    for (;;)
    {
        Apc apc;
        {
            std::lock_guard<std::mutex> g(g_waitLock);
            if (self->apcs.empty())
                break;
            apc = self->apcs.front();
            self->apcs.pop_front();
        }
        apc.fn(apc.arg);
        ran = true;
    }
    return ran;
}

void QueueApc(KThread *thread, void (*fn)(void *), void *arg)
{
    {
        std::lock_guard<std::mutex> g(g_waitLock);
        thread->apcs.push_back({fn, arg});
    }
    g_waitCv.notify_all();
}

// How long the main thread may sleep in a wait before it runs its idle work (main_thread.h: queued calls, the
// Cocoa event pump)
static constexpr std::chrono::milliseconds kMainThreadTick(kMainThreadPumpIntervalMs);

void WakeAllWaiters()
{
    // taking the lock orders this after a waiter's last check of its condition (no lost wakeup)
    {
        std::lock_guard<std::mutex> g(g_waitLock);
    }
    g_waitCv.notify_all();
}

static DWORD WaitImpl(DWORD n, const HANDLE *handles, BOOL waitAll, DWORD ms, BOOL alertable)
{
    KThread *self = CurrentThread();
    KObject *objs[MAXIMUM_WAIT_OBJECTS];
    if (n > MAXIMUM_WAIT_OBJECTS)
    {
        SetError(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    for (DWORD i = 0; i < n; ++i)
    {
        objs[i] = Resolve(handles[i]);
        if (!objs[i] && handles[i] != GetCurrentProcess())
        {
            SetError(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
    }
    if (alertable && RunApcs(self))
        return WAIT_IO_COMPLETION;

    // the main thread runs the calls other threads are waiting on it for, and pumps the Cocoa events, while it
    // waits (main_thread.h)
    const bool mainThread = IsMainThread();
    if (mainThread)
        MainThreadIdle();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms == INFINITE ? 0 : ms);
    std::unique_lock<std::mutex> lk(g_waitLock);
    for (;;)
    {
        if (mainThread && MainThreadIdleDue())
        {
            lk.unlock();
            MainThreadIdle();
            lk.lock();
            continue;
        }
        if (alertable && !self->apcs.empty())
        {
            lk.unlock();
            RunApcs(self);
            return WAIT_IO_COMPLETION;
        }
        if (waitAll)
        {
            bool all = n > 0;
            for (DWORD i = 0; i < n; ++i)
                if (!objs[i] || !IsSignaled(objs[i], self))
                    all = false;
            if (all)
            {
                for (DWORD i = 0; i < n; ++i)
                    Consume(objs[i], self);
                return WAIT_OBJECT_0;
            }
        }
        else
        {
            for (DWORD i = 0; i < n; ++i)
            {
                if (objs[i] && IsSignaled(objs[i], self))
                {
                    Consume(objs[i], self);
                    return WAIT_OBJECT_0 + i;
                }
            }
        }
        if (ms == 0)
            return WAIT_TIMEOUT;
        if (mainThread)
        {
            // wake at least every tick for the idle work
            auto wake = std::chrono::steady_clock::now() + kMainThreadTick;
            if (ms != INFINITE && deadline < wake)
                wake = deadline;
            g_waitCv.wait_until(lk, wake);
            if (ms == INFINITE || std::chrono::steady_clock::now() < deadline)
                continue;
            for (DWORD i = 0; i < n; ++i)
            {
                if (!waitAll && objs[i] && IsSignaled(objs[i], self))
                {
                    Consume(objs[i], self);
                    return WAIT_OBJECT_0 + i;
                }
            }
            return WAIT_TIMEOUT;
        }
        if (ms == INFINITE)
            g_waitCv.wait(lk);
        else if (g_waitCv.wait_until(lk, deadline) == std::cv_status::timeout)
        {
            // final check after the timeout
            for (DWORD i = 0; i < n; ++i)
            {
                if (!waitAll && objs[i] && IsSignaled(objs[i], self))
                {
                    Consume(objs[i], self);
                    return WAIT_OBJECT_0 + i;
                }
            }
            return WAIT_TIMEOUT;
        }
    }
}

// -------- thread entry --------

// Threads whose start routine has returned; their stacks are released once pthread_join confirms they are gone.
struct DeadThread
{
    pthread_t pthread;
    void *stack;
};
static std::mutex g_deadLock;
static std::vector<DeadThread> g_deadThreads;

static void ReapDeadThreads()
{
    std::vector<DeadThread> dead;
    {
        std::lock_guard<std::mutex> g(g_deadLock);
        dead.swap(g_deadThreads);
    }
    for (const DeadThread &d : dead)
    {
        pthread_join(d.pthread, nullptr);
        Arena_FreeStack(d.stack);
    }
}

static void *ThreadTrampoline(void *arg)
{
    KThread *t = (KThread *)arg;
    t_self = t;
    {
        std::unique_lock<std::mutex> lk(g_waitLock);
        while (t->suspendCount > 0)
            g_waitCv.wait(lk);
        t->started = true;
    }
    DWORD code = t->start ? t->start(t->param) : 0;
    {
        std::lock_guard<std::mutex> g(g_waitLock);
        t->exitCode = code;
        t->finished = true;
    }
    g_waitCv.notify_all();
    {
        std::lock_guard<std::mutex> g(g_deadLock);
        g_deadThreads.push_back({ pthread_self(), t->stack });
    }
    t->Unref(); // the running thread's own reference
    return nullptr;
}
} // namespace w32

using namespace w32;

extern "C" {

DWORD GetLastError(void) { return t_lastError; }
void SetLastError(DWORD e) { t_lastError = e; }
unsigned int Sys_Compat_CurrentThreadId(void) { return ThreadSelfId(); }

BOOL CloseHandle(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE || h == GetCurrentProcess() || h == GetCurrentThread())
        return TRUE;
    KObject *o = (KObject *)h;
    o->OnClose();
    o->Unref();
    return TRUE;
}

BOOL DuplicateHandle(HANDLE, HANDLE src, HANDLE, LPHANDLE dst, DWORD, BOOL, DWORD options)
{
    if (!dst)
        return FALSE;
    KObject *o = Resolve(src);
    if (!o)
    {
        *dst = src;
        return TRUE;
    }
    o->Ref();
    *dst = (HANDLE)o;
    if (options & DUPLICATE_CLOSE_SOURCE)
        CloseHandle(src);
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return WaitImpl(1, &h, FALSE, ms, FALSE); }
DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable) { return WaitImpl(1, &h, FALSE, ms, alertable); }
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms) { return WaitImpl(n, h, all, ms, FALSE); }
DWORD WaitForMultipleObjectsEx(DWORD n, const HANDLE *h, BOOL all, DWORD ms, BOOL alertable) { return WaitImpl(n, h, all, ms, alertable); }
DWORD SignalObjectAndWait(HANDLE sig, HANDLE wait, DWORD ms, BOOL alertable)
{
    KObject *o = Resolve(sig);
    if (o && o->type == KType::Event)
        SetEvent(sig);
    else if (o && o->type == KType::Mutex)
        ReleaseMutex(sig);
    else if (o && o->type == KType::Semaphore)
        ReleaseSemaphore(sig, 1, nullptr);
    return WaitImpl(1, &wait, FALSE, ms, alertable);
}

// -------- events --------
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCSTR)
{
    KEvent *e = new KEvent();
    e->manualReset = manual != FALSE;
    e->signaled = initial != FALSE;
    return (HANDLE)e;
}
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR) { return CreateEventA(sa, manual, initial, nullptr); }
HANDLE CreateEventExA(LPSECURITY_ATTRIBUTES sa, LPCSTR name, DWORD flags, DWORD)
{
    return CreateEventA(sa, (flags & CREATE_EVENT_MANUAL_RESET) != 0, (flags & CREATE_EVENT_INITIAL_SET) != 0, name);
}
BOOL SetEvent(HANDLE h)
{
    KObject *o = Resolve(h);
    if (!o || o->type != KType::Event)
        return FALSE;
    {
        std::lock_guard<std::mutex> g(g_waitLock);
        static_cast<KEvent *>(o)->signaled = true;
    }
    g_waitCv.notify_all();
    return TRUE;
}
BOOL ResetEvent(HANDLE h)
{
    KObject *o = Resolve(h);
    if (!o || o->type != KType::Event)
        return FALSE;
    std::lock_guard<std::mutex> g(g_waitLock);
    static_cast<KEvent *>(o)->signaled = false;
    return TRUE;
}
BOOL PulseEvent(HANDLE h)
{
    SetEvent(h);
    sched_yield();
    return ResetEvent(h);
}

// -------- mutexes --------
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL initialOwner, LPCSTR name)
{
    // Named mutexes are used as single-instance guards; one process here, so a fresh one is always "new".
    KMutex *m = new KMutex();
    if (initialOwner)
    {
        m->owner = ThreadSelfId();
        m->recursion = 1;
    }
    SetError(ERROR_SUCCESS);
    (void)name;
    return (HANDLE)m;
}
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR) { return CreateMutexA(sa, initialOwner, nullptr); }
HANDLE OpenMutexA(DWORD, BOOL, LPCSTR)
{
    SetError(ERROR_FILE_NOT_FOUND);
    return nullptr;
}
BOOL ReleaseMutex(HANDLE h)
{
    KObject *o = Resolve(h);
    if (!o || o->type != KType::Mutex)
        return FALSE;
    KMutex *m = static_cast<KMutex *>(o);
    {
        std::lock_guard<std::mutex> g(g_waitLock);
        if (m->owner != ThreadSelfId())
            return FALSE;
        if (--m->recursion == 0)
            m->owner = 0;
    }
    g_waitCv.notify_all();
    return TRUE;
}

// -------- semaphores --------
HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES, LONG initial, LONG maximum, LPCSTR)
{
    KSemaphore *s = new KSemaphore();
    s->count = initial;
    s->maximum = maximum;
    return (HANDLE)s;
}
HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR) { return CreateSemaphoreA(sa, initial, maximum, nullptr); }
HANDLE CreateSemaphoreExA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name, DWORD, DWORD) { return CreateSemaphoreA(sa, initial, maximum, name); }
BOOL ReleaseSemaphore(HANDLE h, LONG n, LPLONG prev)
{
    KObject *o = Resolve(h);
    if (!o || o->type != KType::Semaphore)
        return FALSE;
    KSemaphore *s = static_cast<KSemaphore *>(o);
    {
        std::lock_guard<std::mutex> g(g_waitLock);
        if (prev)
            *prev = s->count;
        if (s->count + n > s->maximum)
            return FALSE;
        s->count += n;
    }
    g_waitCv.notify_all();
    return TRUE;
}

// -------- critical sections --------
static pthread_mutex_t *CsMutex(LPCRITICAL_SECTION cs)
{
    pthread_mutex_t *m = (pthread_mutex_t *)__atomic_load_n(&cs->impl, __ATOMIC_ACQUIRE);
    if (m)
        return m;
    // lazily initialize sections that were zero-initialized instead of InitializeCriticalSection'd
    pthread_mutex_t *nm = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(nm, &a);
    pthread_mutexattr_destroy(&a);
    void *expected = nullptr;
    if (__atomic_compare_exchange_n(&cs->impl, &expected, (void *)nm, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return nm;
    pthread_mutex_destroy(nm);
    free(nm);
    return (pthread_mutex_t *)expected;
}
void InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    memset(cs, 0, sizeof(*cs));
    cs->LockCount = -1;
    CsMutex(cs);
}
BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{
    InitializeCriticalSection(cs);
    cs->SpinCount = spin;
    return TRUE;
}
BOOL InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin, DWORD) { return InitializeCriticalSectionAndSpinCount(cs, spin); }
void DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
    pthread_mutex_t *m = (pthread_mutex_t *)cs->impl;
    if (m)
    {
        pthread_mutex_destroy(m);
        free(m);
    }
    cs->impl = nullptr;
}
void EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    pthread_mutex_t *m = CsMutex(cs);
    if (IsMainThread())
    {
        // the owner may be waiting for a call queued for the main thread (main_thread.h): keep running those
        // while the section is busy (macOS has no pthread_mutex_timedlock)
        for (unsigned tries = 0; pthread_mutex_trylock(m) != 0; ++tries)
        {
            MainThreadIdle();
            if (tries < 64)
                sched_yield();
            else
            {
                struct timespec ts = {0, 50000};
                nanosleep(&ts, nullptr);
            }
        }
    }
    else
        pthread_mutex_lock(m);
    cs->OwningThread = (HANDLE)(uintptr_t)ThreadSelfId();
    cs->RecursionCount++;
}
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (pthread_mutex_trylock(CsMutex(cs)) != 0)
        return FALSE;
    cs->OwningThread = (HANDLE)(uintptr_t)ThreadSelfId();
    cs->RecursionCount++;
    return TRUE;
}
void LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
    if (--cs->RecursionCount == 0)
        cs->OwningThread = nullptr;
    pthread_mutex_unlock(CsMutex(cs));
}
DWORD SetCriticalSectionSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{
    DWORD old = (DWORD)cs->SpinCount;
    cs->SpinCount = spin;
    return old;
}

// -------- SRW locks --------
static pthread_rwlock_t *SrwImpl(PSRWLOCK l)
{
    pthread_rwlock_t *r = (pthread_rwlock_t *)__atomic_load_n(&l->Ptr, __ATOMIC_ACQUIRE);
    if (r)
        return r;
    pthread_rwlock_t *nr = (pthread_rwlock_t *)malloc(sizeof(pthread_rwlock_t));
    pthread_rwlock_init(nr, nullptr);
    void *expected = nullptr;
    if (__atomic_compare_exchange_n(&l->Ptr, &expected, (void *)nr, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return nr;
    pthread_rwlock_destroy(nr);
    free(nr);
    return (pthread_rwlock_t *)expected;
}
void InitializeSRWLock(PSRWLOCK l) { l->Ptr = nullptr; }
void AcquireSRWLockExclusive(PSRWLOCK l) { pthread_rwlock_wrlock(SrwImpl(l)); }
void ReleaseSRWLockExclusive(PSRWLOCK l) { pthread_rwlock_unlock(SrwImpl(l)); }
void AcquireSRWLockShared(PSRWLOCK l) { pthread_rwlock_rdlock(SrwImpl(l)); }
void ReleaseSRWLockShared(PSRWLOCK l) { pthread_rwlock_unlock(SrwImpl(l)); }
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK l) { return pthread_rwlock_trywrlock(SrwImpl(l)) == 0; }
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK l) { return pthread_rwlock_tryrdlock(SrwImpl(l)) == 0; }

// -------- condition variables --------
struct CvImpl
{
    std::mutex m;
    std::condition_variable cv;
    uint64_t generation = 0;
};
static CvImpl *CvGet(PCONDITION_VARIABLE c)
{
    CvImpl *p = (CvImpl *)__atomic_load_n(&c->Ptr, __ATOMIC_ACQUIRE);
    if (p)
        return p;
    CvImpl *np = new CvImpl();
    void *expected = nullptr;
    if (__atomic_compare_exchange_n(&c->Ptr, &expected, (void *)np, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return np;
    delete np;
    return (CvImpl *)expected;
}
void InitializeConditionVariable(PCONDITION_VARIABLE c) { c->Ptr = nullptr; }
} // extern "C"
template <typename Unlock, typename Relock> static BOOL CvSleep(PCONDITION_VARIABLE c, DWORD ms, Unlock unlock, Relock relock)
{
    CvImpl *cv = CvGet(c);
    std::unique_lock<std::mutex> lk(cv->m);
    uint64_t gen = cv->generation;
    unlock();
    bool ok = true;
    if (ms == INFINITE)
        cv->cv.wait(lk, [&] { return cv->generation != gen; });
    else
        ok = cv->cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return cv->generation != gen; });
    lk.unlock();
    relock();
    if (!ok)
        SetError(ERROR_TIMEOUT);
    return ok ? TRUE : FALSE;
}
extern "C" {
BOOL SleepConditionVariableCS(PCONDITION_VARIABLE c, PCRITICAL_SECTION cs, DWORD ms)
{
    LONG depth = cs->RecursionCount;
    return CvSleep(
        c, ms,
        [&] {
            for (LONG i = 0; i < depth; ++i)
                LeaveCriticalSection(cs);
        },
        [&] {
            for (LONG i = 0; i < depth; ++i)
                EnterCriticalSection(cs);
        });
}
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE c, PSRWLOCK l, DWORD ms, ULONG flags)
{
    const bool shared = (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) != 0;
    return CvSleep(
        c, ms, [&] { shared ? ReleaseSRWLockShared(l) : ReleaseSRWLockExclusive(l); },
        [&] { shared ? AcquireSRWLockShared(l) : AcquireSRWLockExclusive(l); });
}
void WakeConditionVariable(PCONDITION_VARIABLE c)
{
    CvImpl *cv = CvGet(c);
    {
        std::lock_guard<std::mutex> g(cv->m);
        cv->generation++;
    }
    cv->cv.notify_one();
}
void WakeAllConditionVariable(PCONDITION_VARIABLE c)
{
    CvImpl *cv = CvGet(c);
    {
        std::lock_guard<std::mutex> g(cv->m);
        cv->generation++;
    }
    cv->cv.notify_all();
}

// WaitOnAddress: polled with a short sleep (only used by optional spin paths).
BOOL WaitOnAddress(volatile void *addr, PVOID cmp, SIZE_T size, DWORD ms)
{
    auto start = std::chrono::steady_clock::now();
    for (;;)
    {
        if (memcmp((const void *)addr, cmp, size) != 0)
            return TRUE;
        if (ms != INFINITE && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(ms))
        {
            SetError(ERROR_TIMEOUT);
            return FALSE;
        }
        usleep(50);
    }
}
void WakeByAddressSingle(PVOID) {}
void WakeByAddressAll(PVOID) {}

// -------- threads --------
HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T stackSize, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags, LPDWORD tid)
{
    KThread *t = new KThread();
    t->start = start;
    t->param = param;
    t->id = g_nextThreadId.fetch_add(4);
    t->suspendCount = (flags & CREATE_SUSPENDED) ? 1 : 0;
    t->Ref(); // reference owned by the running thread
    {
        std::lock_guard<std::mutex> g(g_threadTableLock);
        g_threadTable[t->id] = t;
    }
    ReapDeadThreads();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // The engine's threads keep large structures on the stack, and 64-bit frames are bigger than the 32-bit ones
    // the Windows sizes were chosen for (pointers, spills, alignment): the default (0, the executable's reserve on
    // Windows) gets 8 MB and explicit sizes at least 1 MB. Only touched pages of the reservation use memory.
    size_t ss = stackSize ? stackSize : (8u << 20);
    if (ss < (1u << 20))
        ss = 1u << 20;
    // The stack lives in the arena, so addresses of locals have 32-bit handles too (see arena.cpp).
    size_t usable = 0;
    t->stack = Arena_AllocStack(ss, &usable);
    if (t->stack)
        pthread_attr_setstack(&attr, t->stack, usable);
    else
        pthread_attr_setstacksize(&attr, (ss + 16383) & ~(size_t)16383);
    if (pthread_create(&t->pthread, &attr, ThreadTrampoline, t) != 0)
    {
        pthread_attr_destroy(&attr);
        Arena_FreeStack(t->stack);
        {
            std::lock_guard<std::mutex> g(g_threadTableLock);
            g_threadTable.erase(t->id);
        }
        t->Unref();
        t->Unref();
        SetError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    pthread_attr_destroy(&attr);
    t->machThread = pthread_mach_thread_np(t->pthread);
    // joinable: ReapDeadThreads joins finished threads before their stacks are reused
    if (tid)
        *tid = t->id;
    return (HANDLE)t;
}

HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
DWORD GetCurrentThreadId(void) { return ThreadSelfId(); }
DWORD GetThreadId(HANDLE h)
{
    KObject *o = Resolve(h);
    return (o && o->type == KType::Thread) ? static_cast<KThread *>(o)->id : 0;
}
HANDLE OpenThread(DWORD, BOOL, DWORD id)
{
    KThread *t = FindThread(id);
    if (!t)
    {
        SetError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    t->Ref();
    return (HANDLE)t;
}
DWORD ResumeThread(HANDLE h)
{
    KObject *o = Resolve(h);
    if (!o || o->type != KType::Thread)
        return (DWORD)-1;
    KThread *t = static_cast<KThread *>(o);
    DWORD prev;
    {
        std::lock_guard<std::mutex> g(g_waitLock);
        prev = (DWORD)t->suspendCount;
        if (t->suspendCount > 0)
            t->suspendCount--;
    }
    if (t->started && !t->finished && prev > 0 && t->machSuspended > 0)
    {
        t->machSuspended--;
        thread_resume(t->machThread);
    }
    g_waitCv.notify_all();
    return prev;
}
DWORD SuspendThread(HANDLE h)
{
    KObject *o = Resolve(h);
    if (!o || o->type != KType::Thread)
        return (DWORD)-1;
    KThread *t = static_cast<KThread *>(o);
    DWORD prev;
    {
        std::lock_guard<std::mutex> g(g_waitLock);
        prev = (DWORD)t->suspendCount;
        t->suspendCount++;
    }
    if (t->started && !t->finished && t != CurrentThread())
    {
        if (thread_suspend(t->machThread) == KERN_SUCCESS)
            t->machSuspended++;
    }
    return prev;
}
BOOL TerminateThread(HANDLE, DWORD) { return FALSE; }
void ExitThread(DWORD code)
{
    KThread *t = CurrentThread();
    {
        std::lock_guard<std::mutex> g(g_waitLock);
        t->exitCode = code;
        t->finished = true;
    }
    g_waitCv.notify_all();
    if (!t->adopted)
    {
        std::lock_guard<std::mutex> g(g_deadLock);
        g_deadThreads.push_back({ pthread_self(), t->stack });
    }
    if (!t->adopted)
        t->Unref();
    pthread_exit(nullptr);
}
BOOL GetExitCodeThread(HANDLE h, LPDWORD code)
{
    KObject *o = Resolve(h);
    if (!o || o->type != KType::Thread || !code)
        return FALSE;
    KThread *t = static_cast<KThread *>(o);
    std::lock_guard<std::mutex> g(g_waitLock);
    *code = t->finished ? t->exitCode : STILL_ACTIVE;
    return TRUE;
}
BOOL SetThreadPriority(HANDLE h, int prio)
{
    KObject *o = Resolve(h);
    if (o && o->type == KType::Thread)
        static_cast<KThread *>(o)->priority = prio;
    if (o == CurrentThread())
    {
        qos_class_t q = QOS_CLASS_USER_INITIATED;
        if (prio >= THREAD_PRIORITY_HIGHEST)
            q = QOS_CLASS_USER_INTERACTIVE;
        else if (prio <= THREAD_PRIORITY_LOWEST)
            q = QOS_CLASS_UTILITY;
        pthread_set_qos_class_self_np(q, 0);
    }
    return TRUE;
}
int GetThreadPriority(HANDLE h)
{
    KObject *o = Resolve(h);
    return (o && o->type == KType::Thread) ? static_cast<KThread *>(o)->priority : THREAD_PRIORITY_NORMAL;
}
BOOL SetThreadPriorityBoost(HANDLE, BOOL) { return TRUE; }
DWORD_PTR SetThreadAffinityMask(HANDLE, DWORD_PTR mask) { return mask ? mask : 1; }
DWORD SetThreadIdealProcessor(HANDLE, DWORD) { return 0; }
BOOL GetThreadContext(HANDLE h, LPCONTEXT ctx)
{
    KObject *o = Resolve(h);
    if (!o || o->type != KType::Thread || !ctx)
        return FALSE;
    KThread *t = static_cast<KThread *>(o);
    arm_thread_state64_t st;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(t->machThread, ARM_THREAD_STATE64, (thread_state_t)&st, &count) != KERN_SUCCESS)
        return FALSE;
    ctx->Pc = (DWORD64)arm_thread_state64_get_pc(st);
    ctx->Sp = (DWORD64)arm_thread_state64_get_sp(st);
    ctx->Fp = (DWORD64)arm_thread_state64_get_fp(st);
    ctx->Lr = (DWORD64)arm_thread_state64_get_lr(st);
    for (int i = 0; i < 29; ++i)
        ctx->X[i] = st.__x[i];
    ctx->Eip = (DWORD)ctx->Pc;
    ctx->Esp = (DWORD)ctx->Sp;
    ctx->Ebp = (DWORD)ctx->Fp;
    return TRUE;
}
BOOL SetThreadContext(HANDLE, const CONTEXT *) { return FALSE; }
BOOL QueryThreadCycleTime(HANDLE, PULONG64 cycles)
{
    if (cycles)
        *cycles = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    return TRUE;
}
BOOL GetThreadTimes(HANDLE, LPFILETIME c, LPFILETIME e, LPFILETIME k, LPFILETIME u)
{
    uint64_t t = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) / 100;
    if (c) c->dwLowDateTime = c->dwHighDateTime = 0;
    if (e) e->dwLowDateTime = e->dwHighDateTime = 0;
    if (k) k->dwLowDateTime = k->dwHighDateTime = 0;
    if (u)
    {
        u->dwLowDateTime = (DWORD)t;
        u->dwHighDateTime = (DWORD)(t >> 32);
    }
    return TRUE;
}
HRESULT SetThreadDescription(HANDLE h, PCWSTR name)
{
    if (h == GetCurrentThread() && name)
    {
        char buf[64];
        size_t i = 0;
        for (; name[i] && i < sizeof(buf) - 1; ++i)
            buf[i] = (char)(name[i] < 128 ? name[i] : '?');
        buf[i] = 0;
        pthread_setname_np(buf);
    }
    return S_OK;
}
DWORD QueueUserAPC(PAPCFUNC fn, HANDLE h, ULONG_PTR data)
{
    KObject *o = Resolve(h);
    if (!o || o->type != KType::Thread)
        return 0;
    struct Thunk
    {
        PAPCFUNC fn;
        ULONG_PTR data;
    };
    Thunk *th = new Thunk{fn, data};
    QueueApc(static_cast<KThread *>(o), [](void *p) {
        Thunk *t = (Thunk *)p;
        t->fn(t->data);
        delete t;
    }, th);
    return 1;
}

void Sleep(DWORD ms)
{
    if (IsMainThread() && ms != INFINITE)
    {
        // the main thread runs the calls other threads queued for it, and pumps the Cocoa events, while it sleeps
        // (main_thread.h): the engine waits for the render thread in Sleep loops (R_BeginRegistration) while that
        // thread creates the window
        MainThreadIdle();
        if (ms == 0)
        {
            sched_yield();
            MainThreadIdle();
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        std::unique_lock<std::mutex> lk(g_waitLock);
        for (;;)
        {
            if (MainThreadIdleDue())
            {
                lk.unlock();
                MainThreadIdle();
                lk.lock();
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                break;
            g_waitCv.wait_until(lk, now + kMainThreadTick < deadline ? now + kMainThreadTick : deadline);
        }
        lk.unlock();
        MainThreadIdle();
        return;
    }
    if (ms == 0)
    {
        sched_yield();
        return;
    }
    if (ms == INFINITE)
    {
        for (;;)
            pause();
    }
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
    {
    }
}
DWORD SleepEx(DWORD ms, BOOL alertable)
{
    if (!alertable)
    {
        Sleep(ms);
        return 0;
    }
    KThread *self = CurrentThread();
    if (RunApcs(self))
        return WAIT_IO_COMPLETION;
    const bool mainThread = IsMainThread();
    if (mainThread)
        MainThreadIdle();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms == INFINITE ? 0 : ms);
    std::unique_lock<std::mutex> lk(g_waitLock);
    for (;;)
    {
        if (mainThread && MainThreadIdleDue())
        {
            lk.unlock();
            MainThreadIdle();
            lk.lock();
            continue;
        }
        if (!self->apcs.empty())
        {
            lk.unlock();
            RunApcs(self);
            return WAIT_IO_COMPLETION;
        }
        if (ms == 0)
            return 0;
        if (mainThread)
        {
            auto wake = std::chrono::steady_clock::now() + kMainThreadTick;
            if (ms != INFINITE && deadline < wake)
                wake = deadline;
            g_waitCv.wait_until(lk, wake);
            if (ms != INFINITE && std::chrono::steady_clock::now() >= deadline)
                return 0;
            continue;
        }
        if (ms == INFINITE)
            g_waitCv.wait(lk);
        else if (g_waitCv.wait_until(lk, deadline) == std::cv_status::timeout)
            return 0;
    }
}
BOOL SwitchToThread(void)
{
    sched_yield();
    return TRUE;
}

// -------- TLS / FLS --------
DWORD TlsAlloc(void)
{
    pthread_key_t k;
    if (pthread_key_create(&k, nullptr) != 0)
        return TLS_OUT_OF_INDEXES;
    return (DWORD)k;
}
BOOL TlsFree(DWORD i) { return pthread_key_delete((pthread_key_t)i) == 0; }
LPVOID TlsGetValue(DWORD i)
{
    SetError(ERROR_SUCCESS);
    return pthread_getspecific((pthread_key_t)i);
}
BOOL TlsSetValue(DWORD i, LPVOID v) { return pthread_setspecific((pthread_key_t)i, v) == 0; }
DWORD FlsAlloc(PFLS_CALLBACK_FUNCTION cb)
{
    pthread_key_t k;
    if (pthread_key_create(&k, cb) != 0)
        return TLS_OUT_OF_INDEXES;
    return (DWORD)k;
}
PVOID FlsGetValue(DWORD i) { return pthread_getspecific((pthread_key_t)i); }
BOOL FlsSetValue(DWORD i, PVOID v) { return pthread_setspecific((pthread_key_t)i, v) == 0; }
BOOL FlsFree(DWORD i) { return pthread_key_delete((pthread_key_t)i) == 0; }
DWORD GetCurrentProcessorNumber(void) { return 0; }

// -------- interlocked singly linked list --------
static std::mutex g_slistLock;
void InitializeSListHead(PSLIST_HEADER h) { memset(h, 0, sizeof(*h)); }
PSLIST_ENTRY InterlockedPushEntrySList(PSLIST_HEADER h, PSLIST_ENTRY e)
{
    std::lock_guard<std::mutex> g(g_slistLock);
    PSLIST_ENTRY old = h->s.Next;
    e->Next = old;
    h->s.Next = e;
    h->s.Depth++;
    return old;
}
PSLIST_ENTRY InterlockedPopEntrySList(PSLIST_HEADER h)
{
    std::lock_guard<std::mutex> g(g_slistLock);
    PSLIST_ENTRY e = h->s.Next;
    if (e)
    {
        h->s.Next = e->Next;
        h->s.Depth--;
    }
    return e;
}
PSLIST_ENTRY InterlockedFlushSList(PSLIST_HEADER h)
{
    std::lock_guard<std::mutex> g(g_slistLock);
    PSLIST_ENTRY e = h->s.Next;
    h->s.Next = nullptr;
    h->s.Depth = 0;
    return e;
}
USHORT QueryDepthSList(PSLIST_HEADER h) { return (USHORT)h->s.Depth; }

} // extern "C"
