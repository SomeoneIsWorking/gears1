#include "xma.h"

#include <atomic>
#include <bitset>
#include <chrono>
#include <mutex>
#include <thread>

#include <lucent/log.h>

#include "guest_heap.h"

namespace gears
{

namespace
{

// The register window, and the indices within it that matter. An index is a
// word offset, so a register's address is base + index * 4 (Xenia's own
// arithmetic: r = (addr & 0xFFFF) / 4).
constexpr uint32_t kXmaRegisterBase = 0x7FEA0000;
constexpr uint32_t kRegContextArrayAddress = 0x0600;
constexpr uint32_t kRegNextContextIndex = 0x0607;
constexpr uint32_t kRegContext0Kick = 0x0650;
constexpr uint32_t kRegContext0Lock = 0x0690;
constexpr uint32_t kRegContext0Clear = 0x06A0;
constexpr uint32_t kContextGroupCount = 10; // 10 registers of 32 bits each

// 320 contexts of 64 bytes, which is the console's array and therefore the
// range of indices the title's bitmaps can address.
constexpr uint32_t kContextCount = 320;
constexpr uint32_t kContextSize = 64;

uint32_t g_contextArray = 0;

std::mutex g_contextMutex;
std::bitset<kContextCount> g_contextInUse;

// Registers are read by the title with `lwbrx` -- a byte-reversed load, because
// device registers are little-endian -- so a value published here is stored in
// the host's own byte order rather than the guest's. (Confirmed at
// sub_825E8FE0, which reads the context-array register exactly that way, and by
// the writes the title makes: they read back correctly only when interpreted
// little-endian.)
uint32_t* Register(GuestMemory& memory, uint32_t index)
{
    return memory.Translate<uint32_t>(kXmaRegisterBase + index * 4);
}

std::atomic<bool> g_watchStop{false};
std::thread g_watchThread;

// The kick registers are how the title says "decode this context now": one bit
// per context index, across ten 32-bit registers.
//
// Nothing decodes yet, so this thread does not pretend to. What it does is make
// the kicks VISIBLE -- without it, a title that asks for decode and a title that
// never asks look identical, which is the same ambiguity that hid the audio
// blocker for two sessions. The bits are left standing rather than consumed,
// because clearing a register the hardware may not clear would be a guess about
// a contract nothing has established yet.
//
// POLLING IS AN INSTRUMENT HERE, NOT A DESIGN. A kick is a register WRITE, and
// the title overwrites the register with a fresh bitmap each time, so a poll
// can only report which contexts it has ever caught set -- two kicks inside one
// poll interval are indistinguishable from one. That is fine for "is the title
// asking at all", and NOT fine for driving a decoder, which must see every
// kick. The real mechanism is to trap the write: the recompiler already routes
// device stores through PPC_MM_STORE_U32, which is #ifndef-guarded and exists
// precisely so a runtime can define it. Any decoder work starts there.
void WatchKicks(GuestMemory& memory)
{
    uint32_t seen[kContextGroupCount] = {};
    uint64_t reported = 0;
    while (!g_watchStop.load(std::memory_order_relaxed))
    {
        for (uint32_t group = 0; group < kContextGroupCount; ++group)
        {
            const uint32_t kick = *Register(memory, kRegContext0Kick + group);
            const uint32_t fresh = kick & ~seen[group];
            if (fresh == 0)
                continue;
            seen[group] |= fresh;

            for (uint32_t bit = 0; bit < 32; ++bit)
            {
                if ((fresh & (1u << bit)) == 0)
                    continue;
                const uint32_t index = group * 32 + bit;
                // Only the first few, then a count: a title that streams audio
                // kicks continuously, and a log line per kick would drown the
                // run it is meant to explain.
                if (++reported <= 8)
                    lucent::info("xma", "context {} kicked (register {:#x}), and nothing"
                        " decodes it", index, kRegContext0Kick + group);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (reported > 8)
        lucent::info("xma", "{} distinct contexts kicked, none decoded", reported);
}

} // namespace

bool SetupXmaRegisters(GuestMemory& memory)
{
    // The console's kernel allocates the array as uncached physical memory and
    // writes its physical address to the register. Virtual and physical are the
    // same view in this runtime (MmGetPhysicalAddress is the identity), so the
    // address the title reads from the register is the same one it will see
    // back from XMACreateContext -- which is what its index arithmetic needs.
    uint32_t size = kContextCount * kContextSize;
    g_contextArray = PhysicalHeap().Allocate(0, size, kMemCommit);
    if (g_contextArray == 0)
    {
        lucent::error("xma", "could not allocate the {}-context array", kContextCount);
        return false;
    }
    if ((g_contextArray & 0x3F) != 0)
    {
        // The title recovers an index by subtracting the base and shifting by
        // 6, so a misaligned base makes every index wrong by a fraction of a
        // slot. Better to fail loudly than to decode into the wrong context.
        lucent::error("xma", "context array at {:#x} is not 64-byte aligned",
            g_contextArray);
        return false;
    }

    std::memset(memory.Translate<uint8_t>(g_contextArray), 0, size);
    *Register(memory, kRegContextArrayAddress) = g_contextArray;
    *Register(memory, kRegNextContextIndex) = 1;

    lucent::info("xma", "context array at {:#x} ({} x {} bytes), published in"
        " register {:#x}", g_contextArray, kContextCount, kContextSize,
        kRegContextArrayAddress);

    g_watchThread = std::thread(WatchKicks, std::ref(memory));
    return true;
}

uint32_t AllocateXmaContext()
{
    std::lock_guard<std::mutex> guard(g_contextMutex);
    if (g_contextArray == 0)
        return 0;
    for (uint32_t i = 0; i < kContextCount; ++i)
    {
        if (g_contextInUse[i])
            continue;
        g_contextInUse[i] = true;
        const uint32_t pointer = g_contextArray + i * kContextSize;
        lucent::debug("xma", "context {} allocated at {:#x}", i, pointer);
        return pointer;
    }
    lucent::error("xma", "all {} contexts are in use", kContextCount);
    return 0;
}

void ReleaseXmaContext(uint32_t guestPointer)
{
    std::lock_guard<std::mutex> guard(g_contextMutex);
    if (g_contextArray == 0 || guestPointer < g_contextArray)
        return;
    const uint32_t offset = guestPointer - g_contextArray;
    if (offset % kContextSize != 0 || offset / kContextSize >= kContextCount)
    {
        lucent::warn("xma", "release of {:#x}, which is not a context in the array",
            guestPointer);
        return;
    }
    g_contextInUse[offset / kContextSize] = false;
}

} // namespace gears
