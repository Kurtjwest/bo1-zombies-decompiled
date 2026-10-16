#pragma once
// Calls that have to run on the main thread, made from any thread.
//
// AppKit objects (windows, views, the cursor, screens) may only be used on the main thread, but the engine creates and
// manages its window from the render thread: R_BeginRegistrationInternal -> R_CreateWindow runs on the backend
// thread, while the main thread waits for it in R_BeginRegistration. A call from another thread is therefore queued,
// the main thread is woken, and the caller waits until the main thread has run it. The main thread runs queued calls
// from the Cocoa event pump and from every blocking call of the Win32 layer (waits, Sleep, contended critical
// sections), which is where it is when another thread needs it.

#include <type_traits>
#include <utility>

namespace w32
{
// Marks the calling thread as the main thread (PlatformInit, first thing in main()).
void SetMainThread();
bool IsMainThread();

// Runs fn(ctx) on the main thread and returns once it has run; runs it directly on the main thread itself (and before
// SetMainThread, when there is no main thread to wait for yet).
void RunOnMainThread(void (*fn)(void *), void *ctx);
template <typename F> void RunOnMainThread(F &&f)
{
    using Fn = std::remove_reference_t<F>;
    RunOnMainThread([](void *p) { (*(Fn *)p)(); }, (void *)&f);
}

// Queues fn(ctx) for the main thread without waiting for it (state changes nobody waits on, such as showing the
// cursor); runs it directly on the main thread. Queued calls run in order, so a later RunOnMainThread from the same
// thread sees its effect.
void PostToMainThread(void (*fn)(void *), void *ctx);
template <typename F> void PostToMainThread(F &&f)
{
    using Fn = std::decay_t<F>;
    PostToMainThread(
        [](void *p) {
            Fn *fn = (Fn *)p;
            (*fn)();
            delete fn;
        },
        (void *)new Fn(std::forward<F>(f)));
}

// On the main thread: runs the queued calls (cheap when there are none). A no-op on other threads.
void ServiceMainThreadCalls();
bool MainThreadCallsPending();

// Called whenever a call is queued, to wake the main thread wherever it waits (the Win32 layer's condition variable,
// the Cocoa run loop).
void AddMainThreadWaker(void (*wake)(void));

// The Cocoa event queue belongs to the main thread too, but the engine pumps window messages on its render thread
// (RB_SwapBuffers' message loop; the main thread only runs the game). The main thread therefore drains the Cocoa
// events whenever it waits, at most every kMainThreadPumpIntervalMs, through this hook (user32 layer); the window
// messages it produces are queued for the render thread's PeekMessage, as Windows queues them for the window's thread.
void SetMainThreadPumpHook(void (*pump)(void));
constexpr unsigned kMainThreadPumpIntervalMs = 4;
// The main thread's wait points: queued calls, then the event pump when it is due. A no-op on other threads.
void MainThreadIdle();
// On the main thread: whether MainThreadIdle has anything to do now (queued calls, or the pump is due).
bool MainThreadIdleDue();
} // namespace w32
