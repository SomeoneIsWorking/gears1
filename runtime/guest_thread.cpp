#include "guest_thread.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_heap.h"

namespace gears
{

namespace
{
// Offsets within the KPCR that guest code is known to read. Only the fields the
// game actually touches are populated; the rest stays zeroed so an unexpected
// read shows up as a zero rather than as plausible-looking noise.
constexpr uint32_t kPcrTlsPtr = 0x000;
constexpr uint32_t kPcrStackBase = 0x008;
constexpr uint32_t kPcrStackLimit = 0x00C;
constexpr uint32_t kPcrCurrentThread = 0x100;

// The KPRCB is embedded in the KPCR at 0x100, and its current_cpu byte is at
// +0x0C -- so KPCR+0x10C is "which hardware thread am I on", and the console's
// scheduler maintains it. Nothing here can, so it is written once per thread.
//
// This is not cosmetic. The title's audio worker (sub_825EAA68) is a rendezvous
// barrier: each participant does `store8(array + kpcr[0x10C], 1)` and then spins
// until the whole 64-bit array matches the mask of participating cores. Left at
// zero for every thread, every participant checks in at slot 0, the mask is
// never reached, and the barrier spins forever -- which is precisely where the
// audio worker sat while the pump waited on it (catalog #40).
constexpr uint32_t kPcrCurrentCpu = 0x10C;

// Two KTHREAD fields the console's scheduler maintains and the title only ever
// reads -- nothing in the whole image writes either, so if the runtime does not
// populate them they stay zero forever.
//
// The title's adaptive lock (sub_82221A68) spins on a ticket counter and asks
// sub_8222F460 each pass whether to keep spinning. That decision is
// `tick - tickAtLastProgress < 5000`, read from kThreadTickCount. Left at zero
// the difference is always zero, so the answer is always "keep spinning" and
// the lock can never fall through to its blocking path -- which is exactly how
// startup hung, with one thread burning a core while the rest waited on work it
// would have produced.
//
// kThreadProcessorNumber is read by sub_827A7B08 so the same lock can tell
// whether it is spinning on the core that holds the lock, where spinning cannot
// possibly help.
constexpr uint32_t kThreadTickCount = 0x058;
constexpr uint32_t kThreadProcessorNumber = 0x14C;

// The console has six hardware threads and the title asks for specific ones via
// KeSetAffinityThread. The runtime does not honour affinity -- the host
// scheduler places threads -- but the number reported here must still be
// distinct per thread, or the lock concludes every waiter is on the holder's
// core and stops spinning immediately.
constexpr uint32_t kHardwareThreadCount = 6;
std::atomic<uint32_t> g_nextProcessorNumber{0};

constexpr uint32_t kPcrSize = 0x1000;
constexpr uint32_t kThreadSize = 0x1000;
constexpr uint32_t kTlsSize = 0x1000;

void Store32(GuestMemory& memory, uint32_t address, uint32_t value)
{
    *memory.Translate<uint32_t>(address) = ByteSwap(value);
}

void Store8(GuestMemory& memory, uint32_t address, uint8_t value)
{
    *memory.Translate<uint8_t>(address) = value;
}

// Every live thread block, so the tick can be published into all of them. The
// console updates a thread's accounting as it is scheduled; there is no
// equivalent hook here, so a single writer refreshes them all instead.
std::mutex g_threadBlockMutex;
std::vector<uint32_t> g_threadBlocks;
std::once_flag g_tickerStarted;

void PublishTicks(GuestMemory& memory)
{
    const auto start = std::chrono::steady_clock::now();
    for (;;)
    {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const uint32_t tick = uint32_t(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

        {
            std::lock_guard<std::mutex> guard(g_threadBlockMutex);
            for (uint32_t thread : g_threadBlocks)
                Store32(memory, thread + kThreadTickCount, tick);
        }

        // A millisecond is the granularity the count is expressed in, so there
        // is nothing to gain from refreshing faster than it can change.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
} // namespace

namespace
{
// "host" rather than empty, so a line from a thread that never entered guest
// code is labelled as such instead of looking like a missing field.
thread_local std::string t_guestThreadName = "host";
} // namespace

void SetGuestThreadName(std::string name)
{
    t_guestThreadName = std::move(name);
}

const char* GuestThreadName()
{
    return t_guestThreadName.c_str();
}

bool CreateGuestThreadBlock(GuestMemory& memory, uint32_t stackSize, GuestThreadBlock& out)
{
    uint32_t pcrSize = kPcrSize;
    uint32_t threadSize = kThreadSize;
    uint32_t tlsSize = kTlsSize;

    const uint32_t pcr = TitleHeap().Allocate(0, pcrSize, kMemCommit);
    const uint32_t thread = TitleHeap().Allocate(0, threadSize, kMemCommit);
    const uint32_t tls = TitleHeap().Allocate(0, tlsSize, kMemCommit);
    uint32_t stack = TitleHeap().Allocate(0, stackSize, kMemCommit);

    if (pcr == 0 || thread == 0 || tls == 0 || stack == 0)
    {
        lucent::error("thread", "could not allocate the guest thread block");
        return false;
    }

    out.pcrAddress = pcr;
    out.threadAddress = thread;
    out.stackLimit = stack;
    out.stackBase = stack + stackSize;

    Store32(memory, pcr + kPcrTlsPtr, tls);
    Store32(memory, pcr + kPcrStackBase, out.stackBase);
    Store32(memory, pcr + kPcrStackLimit, out.stackLimit);
    Store32(memory, pcr + kPcrCurrentThread, thread);

    const uint32_t processorNumber =
        g_nextProcessorNumber.fetch_add(1) % kHardwareThreadCount;
    Store32(memory, thread + kThreadProcessorNumber, processorNumber);
    Store32(memory, thread + kThreadTickCount, 0);
    Store8(memory, pcr + kPcrCurrentCpu, uint8_t(processorNumber));

    {
        std::lock_guard<std::mutex> guard(g_threadBlockMutex);
        g_threadBlocks.push_back(thread);
    }

    // Started with the first thread block rather than at boot, so the ticker
    // never runs without a block to write into.
    std::call_once(g_tickerStarted, [&memory] {
        std::thread(PublishTicks, std::ref(memory)).detach();
        lucent::info("thread", "publishing the scheduler tick into thread blocks");
    });

    lucent::info("thread", "thread block: pcr={:#x} thread={:#x} tls={:#x} stack={:#x}..{:#x}",
        pcr, thread, tls, out.stackLimit, out.stackBase);
    return true;
}

} // namespace gears
