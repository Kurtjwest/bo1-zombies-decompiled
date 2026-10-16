#pragma once
// Internal kernel-object model of the Win32 compatibility layer.
#include <windows.h>
#include <mach/mach.h>
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

// Base of everything a HANDLE points at. Handles are plain pointers to these objects.
struct KObject
{
    KType type;
    std::atomic<int> refs{1};
    explicit KObject(KType t) : type(t) {}
    virtual ~KObject();
    virtual void OnClose() {}
    void Ref();
    void Unref();
};

// All waitable state is guarded by g_waitLock; any change that can satisfy a wait notifies g_waitCv.
extern std::mutex g_waitLock;
extern std::condition_variable g_waitCv;

struct KEvent : KObject
{
    bool manualReset = false;
    bool signaled = false;
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
    mach_port_t machThread = MACH_PORT_NULL;
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
KThread *CurrentThread();
KThread *FindThread(DWORD id);
void ForEachThread(void (*fn)(KThread *, void *), void *ctx);
void QueueApc(KThread *thread, void (*fn)(void *), void *arg);
void SetError(DWORD e);
DWORD ErrnoToWin32(int e);

// Paths: Windows-style path -> host path (separators, drive letters, case-insensitive resolution).
std::string HostPath(const char *winPath);
bool ResolveCaseInsensitive(const std::string &path, std::string &out);

// Command line captured by main()
void SetCommandLine(int argc, char **argv);
} // namespace w32
