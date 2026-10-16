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

// On the main thread: runs the queued calls (cheap when there are none). A no-op on other threads.
void ServiceMainThreadCalls();
bool MainThreadCallsPending();

// Called whenever a call is queued, to wake the main thread wherever it waits (the Win32 layer's condition variable,
// the Cocoa run loop).
void AddMainThreadWaker(void (*wake)(void));
} // namespace w32
