// Calls that have to run on the main thread (see main_thread.h).

#include "main_thread.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace w32
{
namespace
{
struct MainCall
{
    void (*fn)(void *);
    void *ctx;
    bool done;
};

thread_local bool t_isMainThread;
std::atomic<bool> g_haveMainThread{false};
std::mutex g_callLock;
std::condition_variable g_callDone;
std::deque<MainCall *> g_calls;
std::atomic<int> g_pendingCalls{0};

constexpr int kMaxWakers = 4;
void (*g_wakers[kMaxWakers])(void);
std::atomic<int> g_wakerCount{0};
std::mutex g_wakerLock;
} // namespace

void SetMainThread()
{
    t_isMainThread = true;
    g_haveMainThread.store(true, std::memory_order_release);
}

bool IsMainThread() { return t_isMainThread; }

bool MainThreadCallsPending() { return g_pendingCalls.load(std::memory_order_acquire) != 0; }

void AddMainThreadWaker(void (*wake)(void))
{
    std::lock_guard<std::mutex> g(g_wakerLock);
    const int n = g_wakerCount.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        if (g_wakers[i] == wake)
            return;
    if (n < kMaxWakers)
    {
        g_wakers[n] = wake;
        g_wakerCount.store(n + 1, std::memory_order_release);
    }
}

void RunOnMainThread(void (*fn)(void *), void *ctx)
{
    if (t_isMainThread || !g_haveMainThread.load(std::memory_order_acquire))
    {
        fn(ctx);
        return;
    }
    MainCall call{fn, ctx, false};
    {
        std::lock_guard<std::mutex> g(g_callLock);
        g_calls.push_back(&call);
        g_pendingCalls.fetch_add(1, std::memory_order_release);
    }
    const int n = g_wakerCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        g_wakers[i]();
    std::unique_lock<std::mutex> lk(g_callLock);
    g_callDone.wait(lk, [&] { return call.done; });
}

void ServiceMainThreadCalls()
{
    if (!t_isMainThread || !g_pendingCalls.load(std::memory_order_acquire))
        return;
    for (;;)
    {
        MainCall *c;
        {
            std::lock_guard<std::mutex> g(g_callLock);
            if (g_calls.empty())
                return;
            c = g_calls.front();
            g_calls.pop_front();
            g_pendingCalls.fetch_sub(1, std::memory_order_release);
        }
        c->fn(c->ctx);
        {
            std::lock_guard<std::mutex> g(g_callLock);
            c->done = true;
        }
        g_callDone.notify_all();
    }
}
} // namespace w32
