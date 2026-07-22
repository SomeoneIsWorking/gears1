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
#include "guest_heap.h"
#include "guest_memory.h"

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

// Guest heap. The allocator has to RECYCLE freed address space: a bump
// allocator that never reuses anything exhausted the 512 MiB physical heap
// after ~160 s of gameplay. These tests are about that property and about the
// things reuse can get wrong -- alignment, coalescing, and handing out a block
// that is still owned.
void TestHeapReuse()
{
    gears::GuestMemory memory;
    Check(memory.Reserve(), "heap: guest memory reserves");

    // A small window inside the 64 KiB-page range, well away from the image.
    gears::GuestHeap heap(memory, 0x40000000, 0x00100000); // 1 MiB

    uint32_t size = 0x10000;
    const uint32_t a = heap.Allocate(0, size, gears::kMemCommit | gears::kMemLargePages);
    Check(a == 0x40000000, "heap: first allocation starts at the base");
    Check(size == 0x10000, "heap: size is rounded to the page size");

    // Freeing and reallocating the same shape must return the SAME address.
    // If it walks forwards, the heap leaks.
    Check(heap.Free(a), "heap: free of a live region succeeds");
    size = 0x10000;
    const uint32_t b = heap.Allocate(0, size, gears::kMemCommit | gears::kMemLargePages);
    Check(b == a, "heap: a freed region is handed out again");

    // The whole heap must be reachable through repeated churn, which it is
    // only if neighbouring frees coalesce.
    Check(heap.Free(b), "heap: free again");
    for (int cycle = 0; cycle < 64; ++cycle)
    {
        uint32_t sizes[8];
        uint32_t addrs[8];
        for (int i = 0; i < 8; ++i)
        {
            sizes[i] = 0x20000;
            addrs[i] = heap.Allocate(0, sizes[i], gears::kMemCommit | gears::kMemLargePages);
        }
        for (int i = 0; i < 8; ++i)
        {
            if (addrs[i] == 0)
            {
                Check(false, "heap: churn must not exhaust a heap that fits the working set");
                return;
            }
        }
        // Free in reverse so the coalescing has to merge forwards as well.
        for (int i = 7; i >= 0; --i)
            heap.Free(addrs[i]);
    }

    const gears::GuestHeap::Usage after = heap.GetUsage();
    Check(after.allocated == 0, "heap: nothing is live after freeing everything");
    Check(after.free == 0x00100000, "heap: all bytes are back on the free list");
    Check(after.freeBlocks == 1, "heap: the free list coalesces back to one block");
    Check(after.peak <= 0x00100000, "heap: peak never exceeds the heap");

    // Alignment must be honoured out of a reused block, not just out of fresh
    // space at the cursor.
    uint32_t small = 0x1000;
    const uint32_t head = heap.Allocate(0, small, gears::kMemCommit); // 4 KiB page
    Check(head == 0x40000000, "heap: 4 KiB allocation at the base");
    uint32_t aligned = 0x1000;
    const uint32_t alignedAddr = heap.Allocate(0, aligned, gears::kMemCommit, 0x10000);
    Check(alignedAddr != 0 && (alignedAddr & 0xFFFF) == 0,
        "heap: alignment is honoured when carving a free block");
    heap.Free(head);
    heap.Free(alignedAddr);

    // Two live allocations must never overlap.
    uint32_t s1 = 0x30000, s2 = 0x30000;
    const uint32_t x = heap.Allocate(0, s1, gears::kMemCommit | gears::kMemLargePages);
    const uint32_t y = heap.Allocate(0, s2, gears::kMemCommit | gears::kMemLargePages);
    Check(x != 0 && y != 0, "heap: two allocations succeed");
    Check(x + s1 <= y || y + s2 <= x, "heap: live allocations do not overlap");
    heap.Free(x);
    heap.Free(y);

    // A fixed allocation inside a live reservation is a re-commit and must not
    // create a second bookkeeping entry -- otherwise freeing the reservation
    // leaves a stale entry that later releases space somebody else owns.
    uint32_t reserveSize = 0x40000;
    const uint32_t reserved = heap.Allocate(0, reserveSize,
        gears::kMemReserve | gears::kMemLargePages);
    Check(reserved != 0, "heap: reservation succeeds");
    const uint32_t liveBefore = heap.GetUsage().regions;
    uint32_t commitSize = 0x10000;
    const uint32_t committed = heap.Allocate(reserved + 0x10000, commitSize,
        gears::kMemCommit | gears::kMemLargePages);
    Check(committed == reserved + 0x10000, "heap: re-commit returns the requested address");
    Check(heap.GetUsage().regions == liveBefore,
        "heap: re-commit inside a reservation adds no second region");
    Check(heap.Free(reserved), "heap: the reservation frees as one region");
    Check(heap.GetUsage().allocated == 0, "heap: reservation accounting balances");

    // Exhaustion must still be reported rather than wrapping or overlapping.
    uint32_t huge = 0x00200000;
    Check(heap.Allocate(0, huge, gears::kMemCommit | gears::kMemLargePages) == 0,
        "heap: an allocation larger than the heap fails");

    memory.Release();
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
    TestHeapReuse();

    if (g_failures == 0)
    {
        printf("all runtime logic tests passed\n");
        return 0;
    }

    printf("%d runtime logic test(s) FAILED\n", g_failures);
    return 1;
}
