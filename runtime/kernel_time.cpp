// Timebase and clock imports.
#include "import_stub.h"

#include <chrono>

#include <byteswap.h>

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
} // namespace

void __imp__KeQueryPerformanceFrequency(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = kTimebaseFrequency;
}

// 100 ns units since 1601-01-01, the Windows epoch the console inherited.
void __imp__KeQuerySystemTime(PPCContext& __restrict ctx, uint8_t* base)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const uint64_t unix100ns =
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()) / 100;
    constexpr uint64_t kUnixEpochIn100ns = 116444736000000000ull;
    const uint64_t fileTime = unix100ns + kUnixEpochIn100ns;

    if (ctx.r3.u32 != 0)
        *reinterpret_cast<uint64_t*>(base + ctx.r3.u32) = ByteSwap(fileTime);
}
