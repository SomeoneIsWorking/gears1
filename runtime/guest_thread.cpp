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
// kThreadId is read by sub_827A7B08 so the same lock can tell whether it is
// spinning on the thread that holds the lock, where spinning cannot help.
constexpr uint32_t kThreadTickCount = 0x058;
constexpr uint32_t kThreadCurrentCpu = 0x0BF;
constexpr uint32_t kThreadId = 0x14C;

// Which hardware thread a thread runs on is chosen by the title -- through
// ExCreateThread's flags or KeSetAffinityThread -- and the runtime records that
// choice rather than inventing one, because guest code indexes per-CPU state
// with it. Threads the title expresses no preference for inherit their
// creator's, which is the console's own rule. (kHardwareThreadCount is in the
// header: the callers of ProcessorNumberFromMask need it to tell a processor
// from the sentinels.)
std::atomic<uint32_t> g_nextProcessorNumber{0};

// The KTHREAD's own id field. Distinct per thread is all the title's
// adaptive lock needs of it, and distinct is what a counter gives.
std::atomic<uint32_t> g_nextThreadIdField{1};

// The processor of the thread currently running guest code, for that
// inheritance rule.
thread_local uint8_t t_currentProcessor = 0;

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

uint8_t ProcessorNumberFromMask(uint32_t mask)
{
    if (mask == 0)
        return kProcessorInherit;

    constexpr uint32_t kEveryProcessor = (1u << kHardwareThreadCount) - 1;
    const uint32_t named = mask & kEveryProcessor;
    if (named == 0)
    {
        // Refused rather than folded back into range. This used to answer
        // `ctz(mask) % 6`, which turned 0x40 into processor 0 and 0x100 into
        // processor 2 -- a processor the title never asked for, indistinguishable
        // afterwards from one it did.
        lucent::warn("thread", "processor mask {:#x} names none of the console's"
            " {} hardware threads", mask, kHardwareThreadCount);
        return kProcessorNone;
    }
    if (named != mask)
    {
        lucent::warn("thread", "processor mask {:#x} has bits outside the console's"
            " {} hardware threads; only {:#x} of it is a processor",
            mask, kHardwareThreadCount, named);
    }

    // THE HIGHEST named processor, and the rule is the console's rather than a
    // choice. Two independent sources agree:
    //   - Xenia's XThread::GetFakeCpuNumber is `7 - lzcnt(proc_mask)`.
    //   - The title's OWN inverse decoder, which reads back the previous
    //     affinity mask the kernel wrote (ppc_recomp.97.cpp, guest 0x82613970):
    //         cntlzw r11,r11 ; subfic r3,r11,31      i.e. 31 - clz
    // Taking the lowest instead is invisible today because every mask this title
    // sends is one-hot -- which is exactly why it had to be settled from the
    // console's rule and not from what currently happens to work.
    const uint8_t cpu = uint8_t(31 - __builtin_clz(named));
    if ((named & (named - 1)) != 0)
    {
        lucent::warn("thread", "processor mask {:#x} names {} hardware threads;"
            " recording the highest, {}", mask, __builtin_popcount(named), cpu);
    }
    return cpu;
}

uint8_t CurrentGuestProcessor()
{
    return t_currentProcessor;
}

void SetCurrentGuestProcessor(uint8_t cpu)
{
    t_currentProcessor = cpu;
}

void SetGuestThreadProcessor(GuestMemory& memory, uint32_t pcrAddress,
                             uint32_t threadObject, uint8_t cpu)
{
    Store8(memory, pcrAddress + kPcrCurrentCpu, cpu);
    if (threadObject != 0)
        Store8(memory, threadObject + kThreadCurrentCpu, cpu);
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

    Store32(memory, thread + kThreadTickCount, 0);
    Store32(memory, thread + kThreadId, g_nextThreadIdField.fetch_add(1));

    // A placeholder until the creator says otherwise. ExCreateThread overwrites
    // it from the title's own request; the host threads the runtime drives into
    // guest code keep this, and only need it to be a legal processor.
    SetGuestThreadProcessor(memory, pcr, thread,
        uint8_t(g_nextProcessorNumber.fetch_add(1) % kHardwareThreadCount));

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
