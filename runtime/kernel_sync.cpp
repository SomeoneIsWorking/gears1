// Process-info and critical-section imports.
#include "import_stub.h"

#include <mutex>
#include <thread>
#include <unordered_map>

#include <byteswap.h>
#include <lucent/log.h>

namespace
{

// X_RTL_CRITICAL_SECTION as the guest sees it. The guest reads these fields
// directly in places, so they are kept accurate even though the actual mutual
// exclusion is done host-side.
struct GuestCriticalSection
{
    uint8_t unknown;
    uint8_t spinCountDiv256;
    uint16_t padding;
    int32_t lockCount;      // big-endian; -1 when unheld
    int32_t recursionCount; // big-endian
    uint32_t owningThread;  // big-endian
};

// Backing the guest's critical sections with real host locks means this stays
// correct once guest threads exist, rather than only under single-threaded
// bring-up. Keyed by guest address because that is the only stable identity a
// guest critical section has.
std::recursive_mutex& HostLockFor(uint32_t guestAddress)
{
    static std::mutex tableLock;
    static std::unordered_map<uint32_t, std::unique_ptr<std::recursive_mutex>> table;

    std::lock_guard<std::mutex> guard(tableLock);
    auto& slot = table[guestAddress];
    if (!slot)
        slot = std::make_unique<std::recursive_mutex>();
    return *slot;
}

GuestCriticalSection* CriticalSectionAt(uint8_t* base, uint32_t address)
{
    return reinterpret_cast<GuestCriticalSection*>(base + address);
}

} // namespace

// X_PROCTYPE_TITLE. The other values (idle, system) describe processes a title
// never runs as.
void __imp__KeGetCurrentProcessType(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 1;
}

void __imp__RtlInitializeCriticalSection(PPCContext& __restrict ctx, uint8_t* base)
{
    GuestCriticalSection* cs = CriticalSectionAt(base, ctx.r3.u32);
    cs->unknown = 0;
    cs->spinCountDiv256 = 0;
    cs->padding = 0;
    cs->lockCount = ByteSwap(int32_t(-1));
    cs->recursionCount = 0;
    cs->owningThread = 0;

    HostLockFor(ctx.r3.u32);
    ctx.r3.u64 = 0;
}

void __imp__RtlInitializeCriticalSectionAndSpinCount(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t spinCount = ctx.r4.u32;
    __imp__RtlInitializeCriticalSection(ctx, base);
    CriticalSectionAt(base, ctx.r3.u32)->spinCountDiv256 = uint8_t((spinCount / 256) & 0xFF);
    ctx.r3.u64 = 0;
}

void __imp__RtlEnterCriticalSection(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t address = ctx.r3.u32;
    HostLockFor(address).lock();

    GuestCriticalSection* cs = CriticalSectionAt(base, address);
    cs->lockCount = ByteSwap(int32_t(ByteSwap(cs->lockCount) + 1));
    cs->recursionCount = ByteSwap(int32_t(ByteSwap(cs->recursionCount) + 1));
}

void __imp__RtlTryEnterCriticalSection(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t address = ctx.r3.u32;
    if (!HostLockFor(address).try_lock())
    {
        ctx.r3.u64 = 0;
        return;
    }

    GuestCriticalSection* cs = CriticalSectionAt(base, address);
    cs->lockCount = ByteSwap(int32_t(ByteSwap(cs->lockCount) + 1));
    cs->recursionCount = ByteSwap(int32_t(ByteSwap(cs->recursionCount) + 1));
    ctx.r3.u64 = 1;
}

void __imp__RtlLeaveCriticalSection(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t address = ctx.r3.u32;
    GuestCriticalSection* cs = CriticalSectionAt(base, address);
    cs->lockCount = ByteSwap(int32_t(ByteSwap(cs->lockCount) - 1));
    cs->recursionCount = ByteSwap(int32_t(ByteSwap(cs->recursionCount) - 1));

    HostLockFor(address).unlock();
}

namespace
{
// Critical regions defer kernel APC delivery to the current thread. Nothing
// delivers APCs yet, so there is nothing to defer -- but the depth is tracked
// per thread so an unbalanced pair shows up rather than passing silently.
thread_local int32_t t_criticalRegionDepth = 0;
} // namespace

void __imp__KeEnterCriticalRegion(PPCContext& __restrict ctx, uint8_t*)
{
    ++t_criticalRegionDepth;
}

void __imp__KeLeaveCriticalRegion(PPCContext& __restrict ctx, uint8_t*)
{
    if (--t_criticalRegionDepth < 0)
    {
        lucent::error("kernel", "KeLeaveCriticalRegion without a matching enter");
        t_criticalRegionDepth = 0;
    }
}
