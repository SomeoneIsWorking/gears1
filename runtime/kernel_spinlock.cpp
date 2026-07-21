// Spin locks and IRQL.
//
// These are real locks, not no-ops. Guest threads run concurrently here, so a
// spin lock that did not exclude would be a genuine race -- the console's
// single-processor-per-lock reasoning does not carry over.
#include "import_stub.h"

#include <memory>
#include <mutex>
#include <unordered_map>

#include <lucent/log.h>

namespace
{

std::mutex& HostLockFor(uint32_t guestAddress)
{
    static std::mutex tableLock;
    static std::unordered_map<uint32_t, std::unique_ptr<std::mutex>> table;

    std::lock_guard<std::mutex> guard(tableLock);
    auto& slot = table[guestAddress];
    if (!slot)
        slot = std::make_unique<std::mutex>();
    return *slot;
}

// IRQL is per processor on the console and per thread here. Nothing dispatches
// at raised IRQL, so the level only has to round-trip through raise/lower pairs.
thread_local uint32_t t_irql = 0;

constexpr uint32_t kDpcLevel = 2;

} // namespace

void __imp__KfAcquireSpinLock(PPCContext& __restrict ctx, uint8_t*)
{
    HostLockFor(ctx.r3.u32).lock();
    const uint32_t previous = t_irql;
    t_irql = kDpcLevel;
    ctx.r3.u64 = previous;
}

void __imp__KfReleaseSpinLock(PPCContext& __restrict ctx, uint8_t*)
{
    t_irql = ctx.r4.u32;
    HostLockFor(ctx.r3.u32).unlock();
}

void __imp__KeAcquireSpinLockAtRaisedIrql(PPCContext& __restrict ctx, uint8_t*)
{
    HostLockFor(ctx.r3.u32).lock();
}

void __imp__KeReleaseSpinLockFromRaisedIrql(PPCContext& __restrict ctx, uint8_t*)
{
    HostLockFor(ctx.r3.u32).unlock();
}

void __imp__KeRaiseIrqlToDpcLevel(PPCContext& __restrict ctx, uint8_t*)
{
    const uint32_t previous = t_irql;
    t_irql = kDpcLevel;
    ctx.r3.u64 = previous;
}

void __imp__KfLowerIrql(PPCContext& __restrict ctx, uint8_t*)
{
    t_irql = ctx.r3.u32;
}

// The L2 cache lock partitions cache for the GPU. There is no such partition
// here and nothing reads the result.
void __imp__KeLockL2(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}

void __imp__KeUnlockL2(PPCContext& __restrict ctx, uint8_t*)
{
}

void __imp__KeEnableFpuExceptions(PPCContext& __restrict ctx, uint8_t*)
{
    // The recompiled code manages the host FPU state itself through fpscr; the
    // guest's exception-enable request has nothing to act on.
    ctx.r3.u64 = 0;
}
