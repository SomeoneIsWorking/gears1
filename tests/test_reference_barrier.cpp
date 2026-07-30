// Tests for the reference barrier: the port-side answer to a race the console
// tolerated and this machine does not.
//
// THE SHAPE OF THE PROBLEM. The title's game thread calls a function that holds
// a pointer to a pooled object across the call, re-reading it several times. Its
// rendering thread destroys that object and returns its memory to the pool. On
// the console the two threads were interleaved by a scheduler and a memory
// system that made the window vanishingly small; here it closes on roughly the
// 156th opportunity in a run, every run, and the game thread walks a vtable
// pointer out of recycled memory.
//
// There is no guest protocol left to complete -- the engine has two flags that
// would have been a legitimate seam and NOTHING in the image reads either. So
// the behaviour has to be ported rather than discovered: the port must provide
// the lifetime guarantee the console's timing provided by accident.
//
// WHY DELAY AND NOT SKIP. An earlier attempt suppressed the free outright while
// the object looked referenced. That is a different mechanism with a different
// meaning -- it leaks the block and desynchronises the pool's own bookkeeping,
// so the title's next allocation sees a pool that disagrees with itself. This
// barrier never cancels a free; it only makes the freeing thread WAIT until no
// reader is inside the region that holds the pointer. Every free still happens,
// in the same order, with the same effect.
//
// The tests below drive it with real threads, because a barrier that is only
// ever exercised single-threaded is not tested at all.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "reference_barrier.h"

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

using namespace std::chrono_literals;

// A free of an address nobody is holding must not wait at all. This is the
// common case by a wide margin -- the barrier sits on the pool's free path, so
// if it cost anything when uncontended it would not be affordable there.
void TestUncontendedFreeDoesNotWait()
{
    gears::ReferenceBarrier barrier;
    const auto start = std::chrono::steady_clock::now();
    const bool waited = barrier.WaitUntilUnreferenced(0x1000, 1s);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    Check(!waited, "uncontended: reports that it did not have to wait");
    Check(elapsed < 100ms, "uncontended: and returns promptly");
    Check(barrier.TimeoutCount() == 0, "uncontended: no timeout was recorded");
}

// The case the barrier exists for: a reader is inside the region holding the
// block, and the free must not proceed until it leaves.
void TestFreeWaitsForAnActiveReader()
{
    gears::ReferenceBarrier barrier;
    std::atomic<bool> readerInside{false};
    std::atomic<bool> readerLeft{false};
    std::atomic<bool> freeReturnedBeforeReaderLeft{false};

    std::thread reader([&] {
        gears::ReaderScope scope(barrier, 0x2000);
        readerInside = true;
        std::this_thread::sleep_for(300ms);
        readerLeft = true;
    });

    while (!readerInside)
        std::this_thread::yield();

    std::thread freer([&] {
        barrier.WaitUntilUnreferenced(0x2000, 5s);
        if (!readerLeft)
            freeReturnedBeforeReaderLeft = true;
    });

    reader.join();
    freer.join();

    Check(!freeReturnedBeforeReaderLeft,
        "contended: the free did not proceed while a reader held the block");
    Check(barrier.TimeoutCount() == 0,
        "contended: and it finished by being released, not by timing out");
}

// A reader holding a DIFFERENT block must not delay this free. Without this the
// barrier degenerates into a global lock between the two threads, which would
// serialise the renderer against the game thread and cost far more than the
// race it fixes.
void TestUnrelatedReaderDoesNotBlock()
{
    gears::ReferenceBarrier barrier;
    std::atomic<bool> inside{false};
    std::atomic<bool> release{false};

    std::thread reader([&] {
        gears::ReaderScope scope(barrier, 0x3000);
        inside = true;
        while (!release)
            std::this_thread::yield();
    });

    while (!inside)
        std::this_thread::yield();

    const auto start = std::chrono::steady_clock::now();
    const bool waited = barrier.WaitUntilUnreferenced(0x4000, 1s);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    release = true;
    reader.join();

    Check(!waited, "unrelated: a reader of another block does not delay the free");
    Check(elapsed < 100ms, "unrelated: and the free returns immediately");
}

// A reader that arrives AFTER the freeing thread started waiting must not
// extend the wait indefinitely. The barrier's contract is "no reader that was
// already inside when the free began", not "no reader ever" -- the latter can
// starve the freeing thread forever under a hot loop, which is a hang rather
// than a fix.
void TestLateReaderDoesNotStarveTheFree()
{
    gears::ReferenceBarrier barrier;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> passes{0};

    // A hot loop that enters and leaves the region continuously, always holding
    // the same block: exactly the starvation shape.
    std::thread churn([&] {
        while (!stop)
        {
            gears::ReaderScope scope(barrier, 0x5000);
            passes.fetch_add(1);
        }
    });

    while (passes.load() < 10)
        std::this_thread::yield();

    const auto start = std::chrono::steady_clock::now();
    const bool waited = barrier.WaitUntilUnreferenced(0x5000, 5s);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    stop = true;
    churn.join();

    Check(elapsed < 5s, "starvation: the free completes despite a churning reader");
    Check(barrier.TimeoutCount() == 0,
        "starvation: and it completes by being released, not by timing out");
    (void)waited;
}

// A reader that never leaves must not hang the process. The barrier gives up
// after its deadline, and -- this is the part that matters -- it COUNTS that,
// so a run where the mechanism silently stopped working is distinguishable from
// a run where it was never needed. A barrier that times out quietly is a
// barrier that has been disabled without anyone noticing.
void TestStuckReaderTimesOutAndIsCounted()
{
    gears::ReferenceBarrier barrier;
    std::atomic<bool> inside{false};
    std::atomic<bool> release{false};

    std::thread stuck([&] {
        gears::ReaderScope scope(barrier, 0x6000);
        inside = true;
        while (!release)
            std::this_thread::sleep_for(1ms);
    });

    while (!inside)
        std::this_thread::yield();

    const bool waited = barrier.WaitUntilUnreferenced(0x6000, 200ms);
    Check(waited, "timeout: the wait is reported as having happened");
    Check(barrier.TimeoutCount() == 1,
        "timeout: and it is COUNTED, so a silently-degraded run is visible");

    release = true;
    stuck.join();
}

// Nested and repeated entry by the same thread on the same block must be
// balanced correctly: the block is unreferenced only when the outermost scope
// closes. The guest function this wraps is reached recursively.
void TestNestedScopesAreBalanced()
{
    gears::ReferenceBarrier barrier;
    std::atomic<bool> innerClosed{false};
    std::atomic<bool> outerClosed{false};
    std::atomic<bool> freedTooEarly{false};
    std::atomic<bool> ready{false};

    std::thread reader([&] {
        gears::ReaderScope outer(barrier, 0x7000);
        {
            gears::ReaderScope inner(barrier, 0x7000);
            ready = true;
            std::this_thread::sleep_for(200ms);
        }
        innerClosed = true;
        std::this_thread::sleep_for(200ms);
        outerClosed = true;
    });

    while (!ready)
        std::this_thread::yield();

    std::thread freer([&] {
        barrier.WaitUntilUnreferenced(0x7000, 5s);
        if (!outerClosed)
            freedTooEarly = true;
    });

    reader.join();
    freer.join();

    Check(innerClosed.load(), "nested: the inner scope did close");
    Check(!freedTooEarly,
        "nested: the free waited for the OUTERMOST scope, not the inner one");
}

// Several readers on the same block: the free waits for all of them.
void TestMultipleReadersOnTheSameBlock()
{
    gears::ReferenceBarrier barrier;
    std::atomic<int> inside{0};
    std::atomic<int> left{0};
    std::atomic<bool> freedTooEarly{false};

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
    {
        readers.emplace_back([&, i] {
            gears::ReaderScope scope(barrier, 0x8000);
            inside.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(100 + i * 80));
            left.fetch_add(1);
        });
    }

    while (inside.load() < 4)
        std::this_thread::yield();

    std::thread freer([&] {
        barrier.WaitUntilUnreferenced(0x8000, 5s);
        if (left.load() != 4)
            freedTooEarly = true;
    });

    for (auto& t : readers)
        t.join();
    freer.join();

    Check(!freedTooEarly, "multiple: the free waited for every reader to leave");
}

// The barrier must report whether it ever did anything. A run in which it never
// engaged and a run in which it is broken produce the same crash-free log
// otherwise, and the first is evidence while the second is a coincidence.
void TestEngagementIsCounted()
{
    gears::ReferenceBarrier barrier;
    Check(barrier.WaitCount() == 0, "engagement: starts at zero");

    barrier.WaitUntilUnreferenced(0x9000, 1s);
    Check(barrier.WaitCount() == 0,
        "engagement: an uncontended free is not counted as an engagement");

    std::atomic<bool> inside{false};
    std::thread reader([&] {
        gears::ReaderScope scope(barrier, 0xA000);
        inside = true;
        std::this_thread::sleep_for(150ms);
    });
    while (!inside)
        std::this_thread::yield();
    barrier.WaitUntilUnreferenced(0xA000, 5s);
    reader.join();

    Check(barrier.WaitCount() == 1,
        "engagement: a real wait IS counted, so 'it never fired' is measurable");
}

} // namespace

int main()
{
    TestUncontendedFreeDoesNotWait();
    TestFreeWaitsForAnActiveReader();
    TestUnrelatedReaderDoesNotBlock();
    TestLateReaderDoesNotStarveTheFree();
    TestStuckReaderTimesOutAndIsCounted();
    TestNestedScopesAreBalanced();
    TestMultipleReadersOnTheSameBlock();
    TestEngagementIsCounted();

    if (g_failures == 0)
    {
        printf("all reference barrier tests passed\n");
        return 0;
    }
    printf("%d reference barrier test(s) FAILED\n", g_failures);
    return 1;
}
