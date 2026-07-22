// Timebase and clock imports.
#include "import_stub.h"

#include "kernel_time.h"

#include <chrono>
#include <thread>

#include <byteswap.h>
#include <lucent/log.h>

namespace
{
// The Xenon timebase runs at 50 MHz. Titles derive frame pacing from this, so
// reporting the real console figure matters even though the host clock differs.
constexpr uint64_t kTimebaseFrequency = 50000000;

uint64_t HostNanoseconds()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 100 ns units since 1601-01-01, shared by KeQuerySystemTime and the
// timestamp bundle so the two clocks cannot disagree.
uint64_t FileTimeNow()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const uint64_t unix100ns =
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()) / 100;
    constexpr uint64_t kUnixEpochIn100ns = 116444736000000000ull;
    return unix100ns + kUnixEpochIn100ns;
}
} // namespace

namespace gears
{

void StartKeTimeStampBundle(GuestMemory& memory, uint32_t guestAddress)
{
    std::thread([&memory, guestAddress] {
        const auto start = std::chrono::steady_clock::now();
        auto* interruptTime = memory.Translate<uint64_t>(guestAddress + 0x00);
        auto* systemTime = memory.Translate<uint64_t>(guestAddress + 0x08);
        auto* tickCount = memory.Translate<uint32_t>(guestAddress + 0x10);

        for (;;)
        {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            const uint64_t elapsed100ns = uint64_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) / 100;

            *interruptTime = ByteSwap(elapsed100ns);
            *systemTime = ByteSwap(FileTimeNow());
            *tickCount = ByteSwap(uint32_t(elapsed100ns / 10000));

            // Millisecond granularity is what the bundle is expressed in;
            // refreshing faster than it can change gains nothing.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();

    lucent::info("time", "KeTimeStampBundle at {:#x} updating at 1 kHz", guestAddress);
}

} // namespace gears

void __imp__KeQueryPerformanceFrequency(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = kTimebaseFrequency;
}

// 100 ns units since 1601-01-01, the Windows epoch the console inherited.
void __imp__KeQuerySystemTime(PPCContext& __restrict ctx, uint8_t* base)
{
    if (ctx.r3.u32 != 0)
        *reinterpret_cast<uint64_t*>(base + ctx.r3.u32) = ByteSwap(FileTimeNow());
}
