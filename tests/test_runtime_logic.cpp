// Tests for the runtime's pure logic — the parts whose behaviour is decided by
// us rather than by the guest, and which were written from knowledge of the
// console's semantics rather than from anything verifiable at the time.
//
// Kernel object semantics are the main target. The difference between a
// notification event and a synchronisation event, and semaphore counting, are
// exactly the kind of thing that "looks right" and silently deadlocks or
// over-releases a title much later.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "ppc_config.h"
#include "ppc_context.h"

#include "kernel_objects.h"
#include "guest_filesystem.h"

// The runtime's guest_memory.cpp references the generated function-mapping
// table. These tests exercise none of it, so an empty table stands in rather
// than linking 176 MB of translated game code into a unit test.
PPCFuncMapping PPCFuncMappings[] = { { 0, nullptr } };

namespace
{

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

using gears::KernelObject;

// A notification event stays signalled until explicitly cleared, so EVERY
// waiter gets through and later waiters do not block. Getting this backwards
// gives a title that stalls on a barrier it already passed.
void TestNotificationEvent()
{
    KernelObject e(KernelObject::Kind::NotificationEvent, false);

    Check(!e.Wait(0), "notification: unsignalled wait must time out");

    e.Set();
    Check(e.Wait(0), "notification: signalled wait succeeds");
    Check(e.Wait(0), "notification: stays signalled for a second waiter");
    Check(e.Wait(0), "notification: and a third");

    e.Clear();
    Check(!e.Wait(0), "notification: cleared wait must time out");
}

// A synchronisation event releases exactly ONE waiter, which consumes the
// signal. If this behaved like a notification event, a title would let several
// threads into a section meant for one.
void TestSynchronisationEvent()
{
    KernelObject e(KernelObject::Kind::SynchronizationEvent, false);

    e.Set();
    Check(e.Wait(0), "synchronisation: first waiter passes");
    Check(!e.Wait(0), "synchronisation: signal must be consumed by that waiter");

    e.Set();
    e.Set();
    Check(e.Wait(0), "synchronisation: re-signalled passes");
    Check(!e.Wait(0), "synchronisation: setting twice does not queue two passes");
}

// Initial state must be honoured — a title that creates an already-signalled
// event and immediately waits should not block.
void TestInitialState()
{
    KernelObject signalled(KernelObject::Kind::NotificationEvent, true);
    Check(signalled.Wait(0), "initially-signalled event must not block");

    KernelObject unsignalled(KernelObject::Kind::NotificationEvent, false);
    Check(!unsignalled.Wait(0), "initially-unsignalled event must block");
}

// Semaphores are counted: N releases admit exactly N waiters, no more.
void TestSemaphoreCounting()
{
    KernelObject s(2, 0); // count 2, no limit

    Check(s.Wait(0), "semaphore: first take");
    Check(s.Wait(0), "semaphore: second take");
    Check(!s.Wait(0), "semaphore: exhausted, third must time out");

    const int32_t previous = s.Release(3);
    Check(previous == 0, "semaphore: Release returns the PREVIOUS count");

    Check(s.Wait(0), "semaphore: after release, take 1");
    Check(s.Wait(0), "semaphore: take 2");
    Check(s.Wait(0), "semaphore: take 3");
    Check(!s.Wait(0), "semaphore: exhausted again");
}

// A limit must cap the count, or repeated releases let more threads through
// than the title intended.
void TestSemaphoreLimit()
{
    KernelObject s(0, 2); // count 0, limit 2
    s.Release(5);         // asks for 5, may only reach 2

    Check(s.Wait(0), "semaphore limit: take 1");
    Check(s.Wait(0), "semaphore limit: take 2");
    Check(!s.Wait(0), "semaphore limit: must cap at the limit, not admit 5");
}

// A blocking wait must actually be released by another thread, not just work
// for the already-signalled case. This is what a guest thread handoff depends
// on, and a broken condition-variable predicate would hang here rather than
// return a wrong value.
void TestCrossThreadWake()
{
    KernelObject e(KernelObject::Kind::SynchronizationEvent, false);
    std::atomic<bool> woke{false};

    std::thread waiter([&] {
        if (e.Wait(-1))
            woke = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(!woke.load(), "cross-thread: waiter must still be blocked before Set");

    e.Set();
    waiter.join();
    Check(woke.load(), "cross-thread: waiter must be released by Set");
}

// Timeouts are in 100 ns units and negative means "wait forever"; a sign error
// here turns every timed wait into an infinite one.
void TestTimeoutUnits()
{
    KernelObject e(KernelObject::Kind::NotificationEvent, false);

    const auto start = std::chrono::steady_clock::now();
    const bool ok = e.Wait(200000); // 200000 * 100ns = 20ms
    const auto elapsed = std::chrono::steady_clock::now() - start;

    Check(!ok, "timeout: must report failure");
    Check(elapsed >= std::chrono::milliseconds(15),
        "timeout: must actually wait ~20ms, not return immediately");
    Check(elapsed < std::chrono::seconds(2),
        "timeout: must not wait far longer than asked (unit error)");
}

// Path translation. The console's file systems are case-insensitive and titles
// are casual about case; a Linux host is not.
void TestPathResolution()
{
    gears::FileSystem fs;
    Check(!fs.HasGameDirectory(), "filesystem: starts with no game directory");

    // Resolve must fail cleanly rather than crash when unconfigured.
    Check(fs.Resolve("\\Device\\Cdrom0\\WarGame\\x.dat").empty(),
        "filesystem: unconfigured resolve returns empty");

    fs.SetGameDirectory("/nonexistent-gears-test-dir");
    Check(fs.Resolve("\\SomeUnmappedDevice\\x.dat").empty(),
        "filesystem: unmapped device returns empty");
    Check(fs.Resolve("\\Device\\Cdrom0\\nope.dat").empty(),
        "filesystem: missing file returns empty");
}

} // namespace

int main()
{
    TestNotificationEvent();
    TestSynchronisationEvent();
    TestInitialState();
    TestSemaphoreCounting();
    TestSemaphoreLimit();
    TestCrossThreadWake();
    TestTimeoutUnits();
    TestPathResolution();

    if (g_failures == 0)
    {
        printf("all runtime logic tests passed\n");
        return 0;
    }

    printf("%d runtime logic test(s) FAILED\n", g_failures);
    return 1;
}
