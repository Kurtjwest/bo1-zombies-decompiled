#pragma once
// Internal kernel-object model of the Win32 compatibility layer.
#include <windows.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <semaphore.h>
#include <signal.h>
#include <ucontext.h>
#endif
#include <pthread.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace w32
{
enum class KType
{
    Event,
    Mutex,
    Semaphore,
    Thread,
    Process,
    File,
    Find,
    Mapping,
    Snapshot,
    Token,
    Other
};

struct KThread;

// Base of everything a HANDLE points at. Handles are plain pointers to these objects.
struct KObject
{
    KType type;
    std::atomic<int> refs{1};
    std::vector<KThread *> waiters; // threads blocked in a wait on this object (g_waitLock)
    explicit KObject(KType t) : type(t) {}
    virtual ~KObject();
    virtual void OnClose() {}
    void Ref();
    void Unref();
};

// All waitable state is guarded by g_waitLock. A thread that has to block adds itself to the waiters of the objects
// it waits for and sleeps on its own condition variable (KThread::wakeCv); a change that can satisfy a wait wakes
// the waiters of that object only (WakeWaitersLocked). (One condition variable for every wait in the process woke
// every waiting thread - all the job queue's workers, the stream, database, sound and server threads - at every
// event the engine set or pulsed, hundreds of times a frame.)
extern std::mutex g_waitLock;
void WakeWaitersLocked(KObject *o); // g_waitLock held

struct KEvent : KObject
{
    bool manualReset = false;
    std::atomic<bool> signaled{false}; // (changed under g_waitLock; read without it by a zero-timeout wait)
    uint64_t pulses = 0; // PulseEvent count: a wait in progress when it changes is released (WaitImpl)
    KEvent() : KObject(KType::Event) {}
};

struct KMutex : KObject
{
    DWORD owner = 0;
    int recursion = 0;
    KMutex() : KObject(KType::Mutex) {}
};

struct KSemaphore : KObject
{
    LONG count = 0;
    LONG maximum = 0;
    KSemaphore() : KObject(KType::Semaphore) {}
};

struct Apc
{
    void (*fn)(void *);
    void *arg;
};

struct KThread : KObject
{
    pthread_t pthread{};
#if defined(__APPLE__)
    mach_port_t machThread = MACH_PORT_NULL;
#else
    // SuspendThread of another thread (kernel32_sync.cpp): a signal parks it in its handler, which saves its
    // registers here for GetThreadContext, until ResumeThread posts 'resume'
    pid_t tid = 0;
    sem_t parked;
    sem_t resume;
    bool suspendSemsReady = false;
    ucontext_t parkedContext;
#endif
    DWORD id = 0;
    LPTHREAD_START_ROUTINE start = nullptr;
    void *param = nullptr;
    int suspendCount = 0;
    int machSuspended = 0;
    bool started = false;
    bool finished = false;
    bool adopted = false;
    DWORD exitCode = STILL_ACTIVE;
    int priority = 0;
    void *stack = nullptr; // arena stack of a CreateThread thread (Arena_AllocStack)
    std::deque<Apc> apcs; // guarded by g_waitLock
    std::condition_variable wakeCv; // this thread's waits block on it (with g_waitLock)
    KThread() : KObject(KType::Thread) {}
};

struct KFile : KObject
{
    int fd = -1;
    bool append = false;
    bool deleteOnClose = false;
    std::string path;
    KFile() : KObject(KType::File) {}
    void OnClose() override;
    ~KFile() override;
};

struct KFind : KObject
{
    std::vector<std::string> names; // matches, in directory order
    std::string dir;
    size_t next = 0;
    KFind() : KObject(KType::Find) {}
};

struct KMapping : KObject
{
    int fd = -1;
    size_t size = 0;
    KMapping() : KObject(KType::Mapping) {}
    ~KMapping() override;
};

struct KSnapshot : KObject
{
    std::vector<DWORD> threadIds;
    size_t next = 0;
    KSnapshot() : KObject(KType::Snapshot) {}
};

struct KGeneric : KObject
{
    explicit KGeneric(KType t) : KObject(t) {}
};

DWORD ThreadSelfId();
// Names the calling thread for debuggers and crash reports (truncated to the system's limit).
void SetCurrentThreadName(const char *name);
#if !defined(__APPLE__)
// The registers of a signal context (kernel32_misc.cpp), for the crash handler and GetThreadContext.
bool ReadSignalContext(const ucontext_t *uc, CONTEXT &ctx);
#endif
KThread *CurrentThread();
KThread *FindThread(DWORD id);
void ForEachThread(void (*fn)(KThread *, void *), void *ctx);
void QueueApc(KThread *thread, void (*fn)(void *), void *arg);
// Wakes the main thread if it is blocked in a Win32 wait (or Sleep): it then runs the calls other threads queued
// for it (main_thread.h).
void WakeAllWaiters();
void SetError(DWORD e);
DWORD ErrnoToWin32(int e);

// Paths: Windows-style path -> host path (separators, drive letters, case-insensitive resolution).
std::string HostPath(const char *winPath);
bool ResolveCaseInsensitive(const std::string &path, std::string &out);

// Command line captured by main()
void SetCommandLine(int argc, char **argv);
} // namespace w32
