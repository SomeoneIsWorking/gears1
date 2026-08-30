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
#include <memory>
#include <thread>
#include <type_traits>

#include "ppc_config.h"
#include "ppc_context.h"

#include "kernel_objects.h"
#include "guest_filesystem.h"
#include "guest_heap.h"
#include "guest_thread.h"
#include "guest_memory.h"

// The runtime's guest_memory.cpp references the generated function-mapping
// table. These tests exercise none of it, so an empty table stands in rather
// than linking 176 MB of translated game code into a unit test.
PPCFuncMapping PPCFuncMappings[] = {{0, nullptr}};

namespace
{

int g_failures = 0;

void Check(bool ok, const char *what)
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

    std::thread waiter(
        [&]
        {
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
    Check(fs.SaveDirectory().empty(),
          "filesystem: no title-neutral fallback save namespace is invented");
    Check(fs.SetSaveNamespace("fixture-title"),
          "filesystem: an exact title namespace activates once");
    Check(!fs.SetSaveNamespace("replacement-title"),
          "filesystem: an active title namespace cannot be replaced");

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
    const uint32_t reserved =
        heap.Allocate(0, reserveSize, gears::kMemReserve | gears::kMemLargePages);
    Check(reserved != 0, "heap: reservation succeeds");
    const uint32_t liveBefore = heap.GetUsage().regions;
    uint32_t commitSize = 0x10000;
    const uint32_t committed =
        heap.Allocate(reserved + 0x10000, commitSize, gears::kMemCommit | gears::kMemLargePages);
    Check(committed == reserved + 0x10000, "heap: re-commit returns the requested address");
    Check(heap.GetUsage().regions == liveBefore,
          "heap: re-commit inside a reservation adds no second region");
    Check(heap.Free(reserved), "heap: the reservation frees as one region");
    Check(heap.GetUsage().allocated == 0, "heap: reservation accounting balances");

    // Recycled address space must come back ZEROED. The pages stay committed
    // across a free on purpose, so without an explicit clear the next owner
    // reads the previous tenant's bytes -- which is what let the title's
    // RtlHeap find a stale block header in a segment it had just committed.
    {
        uint32_t s = 0x10000;
        const uint32_t first = heap.Allocate(0, s, gears::kMemCommit | gears::kMemLargePages);
        Check(first != 0, "heap: allocation for the zero-fill check succeeds");
        uint8_t *p = memory.Base() + first;
        for (uint32_t i = 0; i < 0x10000; ++i)
            p[i] = 0xA5;
        Check(heap.Free(first), "heap: free before reuse");
        s = 0x10000;
        const uint32_t again = heap.Allocate(0, s, gears::kMemCommit | gears::kMemLargePages);
        Check(again == first, "heap: the same address comes back");
        bool clean = true;
        for (uint32_t i = 0; i < 0x10000; ++i)
            if (memory.Base()[again + i] != 0)
                clean = false;
        Check(clean, "heap: recycled address space is handed back zeroed");
        Check(heap.Free(again), "heap: free after the zero-fill check");
    }

    // Zeroing must never run over reserved-but-uncommitted pages, which are
    // PROT_NONE and would fault the runtime. The high-water mark alone does not
    // imply committed: an aligned allocation leaves a head fragment below the
    // mark that was never committed, and first fit hands that fragment out
    // later. This allocates into exactly such a fragment.
    {
        uint32_t head = 0x1000;
        const uint32_t low = heap.Allocate(0, head, gears::kMemCommit); // 4 KiB page at the base
        uint32_t big = 0x1000;
        const uint32_t far = heap.Allocate(0, big, gears::kMemCommit, 0x10000);
        Check(low != 0 && far > low + 0x1000,
              "heap: the aligned allocation leaves an uncommitted head fragment");
        Check(heap.Free(low) && heap.Free(far), "heap: free both sides of the fragment");
        uint32_t frag = 0x2000;
        const uint32_t inFragment = heap.Allocate(0, frag, gears::kMemCommit);
        Check(inFragment != 0, "heap: the never-committed fragment is handed out without faulting");
        bool clean = true;
        for (uint32_t i = 0; i < 0x2000; ++i)
            if (memory.Base()[inFragment + i] != 0)
                clean = false;
        Check(clean, "heap: the never-committed fragment reads back zero");
        Check(heap.Free(inFragment), "heap: free the fragment allocation");
    }

    // A fixed commit that runs past its owner and over a NEIGHBOURING live
    // region must absorb it, not leave a second entry owning the same bytes.
    // The guest commits a run it treats as one in several calls, so our
    // bookkeeping splits it; if the inner entry survives, whichever base is
    // freed first puts memory the other still uses back on the free list.
    {
        uint32_t s1 = 0x10000, s2 = 0x10000;
        const uint32_t lo = heap.Allocate(0, s1, gears::kMemCommit | gears::kMemLargePages);
        const uint32_t hi =
            heap.Allocate(lo + 0x10000, s2, gears::kMemCommit | gears::kMemLargePages);
        Check(lo != 0 && hi == lo + 0x10000, "heap: two adjacent commits land adjacently");
        const uint32_t before = heap.GetUsage().regions;
        uint32_t wide = 0x20000;
        const uint32_t fused = heap.Allocate(lo, wide, gears::kMemCommit | gears::kMemLargePages);
        Check(fused == lo, "heap: the widening commit returns its base");
        Check(heap.GetUsage().regions == before - 1,
              "heap: the widening commit absorbs the neighbour instead of overlapping it");
        Check(heap.Free(lo), "heap: the fused region frees as one");
        Check(heap.GetUsage().allocated == 0, "heap: fused-region accounting balances");
    }

    // Exhaustion must still be reported rather than wrapping or overlapping.
    uint32_t huge = 0x00200000;
    Check(heap.Allocate(0, huge, gears::kMemCommit | gears::kMemLargePages) == 0,
          "heap: an allocation larger than the heap fails");

    memory.Release();
}

// Frees are routed to a heap by ADDRESS, because the guest picks the free
// export from a flag on the object rather than from where the memory came
// from. The console does the same: Xenia's Memory::LookupHeap (memory.cc)
// selects the heap purely from the address, and NtFreeVirtualMemory then
// REFUSES a heap that is not the guest-virtual one instead of releasing from
// it (xboxkrnl_memory.cc NtFreeVirtualMemory_entry).
//
// The addresses here are the runtime's real windows, pinned to names, because
// the point of the test is which window an address belongs to: getting the
// physical window's base wrong would route every GPU buffer's free to the
// title heap and every one of them would report as "unknown".
void TestHeapRouting()
{
    gears::GuestMemory memory;
    Check(memory.Reserve(), "routing: guest memory reserves");
    gears::InitialiseHeaps(memory);

    gears::GuestHeap &title = gears::TitleHeap();
    gears::GuestHeap &physical = gears::PhysicalHeap();
    Check(title.Base() == 0x40000000, "routing: the title heap is the 0x40000000 window");
    Check(physical.Base() == 0xA0000000, "routing: the physical heap is the 0xA0000000 window");

    // Address 0 is the one this test exists for. The guest's D3D resource
    // destructor frees a resource whose data pointer it never filled in, and
    // the title's own free wrappers (sub_82612758 XPhysicalFree,
    // sub_82612658 VirtualFree) pass NULL straight to the kernel with no
    // guard, so a free of 0 is normal traffic. It belongs to no heap.
    Check(gears::HeapForAddress(0) == nullptr, "routing: address 0 belongs to no heap");
    Check(!title.Contains(0), "routing: 0 is not in the title heap");
    Check(!physical.Contains(0), "routing: 0 is not in the physical heap");

    Check(gears::HeapForAddress(0x40000000) == &title,
          "routing: the first byte of the title window routes to the title heap");
    Check(gears::HeapForAddress(0x5FFFFFFF) == &title,
          "routing: the last byte of the title window routes to the title heap");
    Check(gears::HeapForAddress(0x60000000) == nullptr,
          "routing: one byte past the title window belongs to no heap");

    Check(gears::HeapForAddress(0xA0000000) == &physical,
          "routing: the first byte of the physical window routes to the physical heap");
    Check(gears::HeapForAddress(0xBFFFFFFF) == &physical,
          "routing: the last byte of the physical window routes to the physical heap");
    Check(gears::HeapForAddress(0xC0000000) == nullptr,
          "routing: one byte past the physical window belongs to no heap");

    // The image, the stack and the import variables live below the title heap
    // and are not the allocator's to release.
    Check(gears::HeapForAddress(0x82000000) == nullptr,
          "routing: the loaded image belongs to no heap");

    // An address really handed out by one heap must not resolve to the other,
    // which is the case that produced frees reported as unknown.
    uint32_t size = 0x10000;
    const uint32_t gpuBuffer =
        physical.Allocate(0, size, gears::kMemCommit | gears::kMemLargePages);
    Check(gpuBuffer != 0, "routing: a physical allocation succeeds");
    Check(gears::HeapForAddress(gpuBuffer) == &physical,
          "routing: a physical allocation routes back to the physical heap");
    Check(!title.Contains(gpuBuffer), "routing: a physical allocation is not in the title heap");
    Check(physical.Free(gpuBuffer), "routing: the physical allocation frees from its own heap");

    memory.Release();
}

// The console names a processor by a one-hot MASK, in the top byte of
// ExCreateThread's creation flags and again in KeSetAffinityThread's affinity
// argument. Both go through one conversion, because two copies is how the two
// paths end up disagreeing about which of the six hardware threads a title
// asked for -- and the number is guest-visible, indexed into per-CPU tables.
//
// Ground truth is Xenia's GetFakeCpuNumber (xenia/kernel/xthread.cc:157): an
// empty mask means "wherever the parent runs"; otherwise the bit index IS the
// processor number; masks are asserted to name exactly one of six processors
// (`assert_false(proc_mask & 0xC0)`, `assert_true(cpu_number < 6)`).
void TestProcessorNumberFromMask()
{
    // The six the console has, each by name, so a shifted conversion cannot
    // pass: every legal mask must give back its own bit index.
    Check(gears::ProcessorNumberFromMask(0x01) == 0, "affinity: mask 0x01 is processor 0");
    Check(gears::ProcessorNumberFromMask(0x02) == 1, "affinity: mask 0x02 is processor 1");
    Check(gears::ProcessorNumberFromMask(0x04) == 2, "affinity: mask 0x04 is processor 2");
    Check(gears::ProcessorNumberFromMask(0x08) == 3, "affinity: mask 0x08 is processor 3");
    Check(gears::ProcessorNumberFromMask(0x10) == 4, "affinity: mask 0x10 is processor 4");
    Check(gears::ProcessorNumberFromMask(0x20) == 5, "affinity: mask 0x20 is processor 5");

    // An empty mask is not a processor and must not be reported as one. It is
    // the console's "inherit my creator's", which only the caller can resolve;
    // a conversion that answered 0 here would silently pin every such thread to
    // hardware thread 0.
    Check(gears::ProcessorNumberFromMask(0x00) == gears::kProcessorInherit,
          "affinity: an empty mask means inherit, not processor 0");

    // Bits above the console's six name no hardware thread that exists. The
    // previous conversion reduced them modulo six -- 0x40 became processor 0,
    // 0x100 became processor 2 -- which invents a processor the title never
    // asked for, and per-CPU tables then disagree without saying so.
    Check(gears::ProcessorNumberFromMask(0x40) == gears::kProcessorNone,
          "affinity: mask 0x40 names no hardware thread of the six");
    Check(gears::ProcessorNumberFromMask(0x80) == gears::kProcessorNone,
          "affinity: mask 0x80 names no hardware thread of the six");
    Check(gears::ProcessorNumberFromMask(0x100) == gears::kProcessorNone,
          "affinity: mask 0x100 names no hardware thread of the six");
    Check(gears::ProcessorNumberFromMask(0xFFFFFFC0) == gears::kProcessorNone,
          "affinity: a mask entirely outside the six names no hardware thread");

    // The two sentinels have to be distinguishable from each other and from
    // every legal processor, because the callers do different things with them:
    // inherit resolves to the creator's processor, no-processor leaves the
    // thread where it was.
    Check(gears::kProcessorInherit != gears::kProcessorNone,
          "affinity: inherit and no-processor are distinct answers");
    Check(gears::kProcessorInherit >= 6 && gears::kProcessorNone >= 6,
          "affinity: neither sentinel can be mistaken for a processor");

    // A mask naming several processors resolves to the HIGHEST named one. That
    // is the console's rule, not a choice made here, and it comes from two
    // independent places: Xenia's GetFakeCpuNumber (`7 - lzcnt`), and the
    // title's own inverse decoder for the previous-affinity mask the kernel
    // writes back (guest 0x82613970: `cntlzw` then `subfic ...,31`).
    //
    // Every mask this title actually sends is one-hot, so the lowest-bit rule
    // would pass every observed case -- which is precisely why it had to be
    // settled against the console instead of against what currently works. An
    // invisible wrong table that satisfies every lookup is the failure mode
    // this project keeps hitting.
    Check(gears::ProcessorNumberFromMask(0x30) == 5,
          "affinity: a multi-processor mask answers with the HIGHEST it names");
    // 0x144 names bits 8, 6 and 2; only bit 2 is a processor this console has,
    // and the bits above it must be dropped rather than folded back into range.
    Check(gears::ProcessorNumberFromMask(0x144) == 2,
          "affinity: bits outside the six are ignored, not folded back in");
}

// A lookup out of a handle table must hand back OWNERSHIP, not a borrowed
// pointer into the container.
//
// This is the rule the open-file table broke: FindFile (kernel_file.cpp) took
// the table mutex, returned a raw OpenFile*, and released the lock before the
// caller had finished reading or writing through it -- so a concurrent NtClose
// erased and destroyed the object mid-use. Every handle table in this runtime
// has the same shape, and the guest has about twenty threads closing handles
// on threads other than the one using them, so the rule is a property of the
// TABLE, not of one call site.
//
// Two halves, and both matter:
//   - the compile-time half fixes the CONTRACT. A future "optimisation" of
//     Lookup back to `KernelObject*` (no copy, no refcount, looks free) is
//     exactly how the file-table bug was written, and it compiles and passes
//     every functional test. A static_assert is the only thing that catches it
//     before a run does.
//   - the runtime half fixes the BEHAVIOUR: an object a caller is holding must
//     survive a close, while the close must still make it unfindable at once.
void TestHandleLookupOwnsItsResult()
{
    // The contract. `auto` at a call site hides which of these two a table
    // returns; this does not.
    static_assert(std::is_same_v<decltype(gears::Handles().Lookup(uint32_t(0))),
                                 std::shared_ptr<gears::KernelObject>>,
                  "HandleTable::Lookup must return an owning shared_ptr: a raw pointer is"
                  " valid only until the next close on any thread");
    static_assert(std::is_same_v<decltype(gears::LookupByGuestAddress(uint32_t(0))),
                                 std::shared_ptr<gears::KernelObject>>,
                  "LookupByGuestAddress must return an owning shared_ptr for the same reason");

    auto object =
        std::make_shared<gears::KernelObject>(gears::KernelObject::Kind::NotificationEvent, false);
    std::weak_ptr<gears::KernelObject> observer = object;
    const uint32_t handle = gears::Handles().Insert(object);
    object.reset(); // the table is now the only owner, as it is in the runtime

    Check(!observer.expired(), "handle lifetime: the table owns the object");

    // What a guest thread does: look the handle up, then work through it.
    auto held = gears::Handles().Lookup(handle);
    Check(held != nullptr, "handle lifetime: an open handle resolves");

    // ...and, in between, another thread closes it. This is the exact race.
    Check(gears::Handles().Close(handle), "handle lifetime: close reports it removed one");
    Check(gears::Handles().Lookup(handle) == nullptr,
          "handle lifetime: a closed handle must be unfindable IMMEDIATELY --"
          " keeping the caller alive must not keep the handle usable");

    // The held reference must still be a live object. With a raw pointer this
    // is a use-after-free: the Set() below writes through freed memory, and
    // whether it crashes depends on what the allocator has since put there,
    // which is why the file-table version surfaced as an unrelated crash.
    Check(!observer.expired(),
          "handle lifetime: an object a caller is still holding must OUTLIVE the close");
    held->Set();
    Check(held->Wait(0), "handle lifetime: the held object is still functional after the close");

    // And it goes away when the last holder does, not before and not never.
    held.reset();
    Check(observer.expired(),
          "handle lifetime: the object is destroyed once the last holder drops it");
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
    TestHeapRouting();
    TestProcessorNumberFromMask();
    TestHandleLookupOwnsItsResult();

    if (g_failures == 0)
    {
        printf("all runtime logic tests passed\n");
        return 0;
    }

    printf("%d runtime logic test(s) FAILED\n", g_failures);
    return 1;
}
