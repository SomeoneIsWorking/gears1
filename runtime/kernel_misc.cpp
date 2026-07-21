// Privileges, pool allocation and thread-local storage.
#include "import_stub.h"

#include <array>
#include <atomic>

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_heap.h"

namespace
{
constexpr size_t kTlsSlotCount = 64;

// Per guest thread once guest threads exist; per host thread today, which is
// the same thing while the runtime is still single-threaded.
thread_local std::array<uint32_t, kTlsSlotCount> t_tlsSlots{};
std::atomic<size_t> g_nextTlsSlot{0};
} // namespace

// Titles query privileges (online play, media playback, ...) to enable optional
// features. Reporting none is honest for a runtime with no Live backing, and
// the game is expected to cope.
void __imp__XexCheckExecutablePrivilege(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("kernel", "XexCheckExecutablePrivilege({}) -> 0", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

void __imp__ExAllocatePool(PPCContext& __restrict ctx, uint8_t*)
{
    uint32_t size = ctx.r3.u32;
    ctx.r3.u64 = gears::TitleHeap().Allocate(0, size, gears::kMemCommit);
}

void __imp__ExAllocatePoolWithTag(PPCContext& __restrict ctx, uint8_t*)
{
    uint32_t size = ctx.r3.u32;
    ctx.r3.u64 = gears::TitleHeap().Allocate(0, size, gears::kMemCommit);
}

void __imp__ExFreePool(PPCContext& __restrict ctx, uint8_t*)
{
    gears::TitleHeap().Free(ctx.r3.u32);
}

void __imp__KeTlsAlloc(PPCContext& __restrict ctx, uint8_t*)
{
    const size_t slot = g_nextTlsSlot.fetch_add(1);
    if (slot >= kTlsSlotCount)
    {
        lucent::error("kernel", "KeTlsAlloc exhausted {} slots", kTlsSlotCount);
        ctx.r3.u64 = uint32_t(-1); // TLS_OUT_OF_INDEXES
        return;
    }

    lucent::debug("kernel", "KeTlsAlloc -> slot {}", slot);
    ctx.r3.u64 = uint32_t(slot);
}

void __imp__KeTlsFree(PPCContext& __restrict ctx, uint8_t*)
{
    // Slots are never recycled: a title allocates a handful at startup and
    // holds them for its lifetime, so a free list would be machinery with no
    // caller. Freeing one leaves it readable-but-unused.
    ctx.r3.u64 = 1;
}

void __imp__KeTlsGetValue(PPCContext& __restrict ctx, uint8_t*)
{
    const uint32_t slot = ctx.r3.u32;
    ctx.r3.u64 = slot < kTlsSlotCount ? t_tlsSlots[slot] : 0;
}

void __imp__KeTlsSetValue(PPCContext& __restrict ctx, uint8_t*)
{
    const uint32_t slot = ctx.r3.u32;
    if (slot < kTlsSlotCount)
        t_tlsSlots[slot] = ctx.r4.u32;
    ctx.r3.u64 = slot < kTlsSlotCount ? 1 : 0;
}

void __imp__DbgPrint(PPCContext& __restrict ctx, uint8_t* base)
{
    // The format arguments follow the PPC varargs convention; rendering them
    // properly needs the guest stack walked, so for now the format string alone
    // is reported -- enough to see what the title is complaining about.
    const char* format = reinterpret_cast<const char*>(base + ctx.r3.u32);
    lucent::info("guest", "DbgPrint: {}", format);
}

// The console's file-system cache tuning knob. There is no equivalent here and
// nothing depends on the size, so the title's request is simply accepted.
void __imp__FscSetCacheElementCount(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("kernel", "FscSetCacheElementCount({}) -> success", ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// The title registers a routine to run when it is being torn down. Nothing
// tears a title down here yet, so the registration is recorded and the routine
// is never invoked.
void __imp__ExRegisterTitleTerminateNotification(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("kernel", "ExRegisterTitleTerminateNotification(routine={:#x}, create={})",
        ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// A deliberately empty APC routine the kernel exports so callers have
// something valid to point at. Doing nothing is the whole contract.
void __imp__KiApcNormalRoutineNop(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}
