// Launch data: the block a title can be handed by whatever launched it (a
// dashboard, another title). Nothing launches this one with data, so the
// honest answer to every query is "none" -- which is a state the console
// itself produces on a normal boot from disc, not a stand-in for missing work.
#include "import_stub.h"

#include <lucent/log.h>

#include <byteswap.h>

#include "guest_heap.h"

// DWORD XamLoaderGetLaunchDataSize(PDWORD outSize)
void __imp__XamLoaderGetLaunchDataSize(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t outSize = ctx.r3.u32;
    if (outSize != 0)
        *reinterpret_cast<uint32_t*>(base + outSize) = 0;

    lucent::debug("xam", "XamLoaderGetLaunchDataSize -> none");
    ctx.r3.u64 = gears::kErrorNotFound;
}

// DWORD XamLoaderGetLaunchData(PVOID buffer, DWORD size)
//
// Consistent with the size query above: there is nothing to copy, so the
// caller's buffer is left untouched rather than filled with invented bytes.
void __imp__XamLoaderGetLaunchData(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("xam", "XamLoaderGetLaunchData({:#x}, {:#x}) -> none",
        ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = gears::kErrorNotFound;
}

// DWORD XamGetSystemVersion(void)
//
// The dashboard version the title is running under. Reporting zero is what a
// title sees on early/unpatched system software, and it is the one answer that
// cannot make this runtime look like it provides a dashboard feature set it
// does not have.
void __imp__XamGetSystemVersion(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("xam", "XamGetSystemVersion -> 0");
    ctx.r3.u64 = 0;
}

// DWORD XamAlloc(DWORD Flags, DWORD Size, PVOID* Out)
//
// Xam's own heap. It comes from the title heap here: the distinction on
// hardware is which pool the system reserves for its applications, and with no
// Xam applications running there is no second pool to keep separate.
void __imp__XamAlloc(PPCContext& __restrict ctx, uint8_t* base)
{
    uint32_t size = ctx.r4.u32;
    const uint32_t outPtr = ctx.r5.u32;

    const uint32_t address = gears::TitleHeap().Allocate(0, size, gears::kMemCommit);
    if (outPtr != 0)
        *reinterpret_cast<uint32_t*>(base + outPtr) = ByteSwap(address);

    if (address == 0)
    {
        lucent::warn("xam", "XamAlloc({:#x}) failed", ctx.r4.u32);
        ctx.r3.u64 = gears::kErrorNotFound;
        return;
    }

    lucent::debug("xam", "XamAlloc({:#x}) -> {:#x}", ctx.r4.u32, address);
    ctx.r3.u64 = gears::kErrorSuccess;
}

// DWORD XamFree(PVOID Address)
void __imp__XamFree(PPCContext& __restrict ctx, uint8_t*)
{
    gears::TitleHeap().Free(ctx.r3.u32);
    ctx.r3.u64 = gears::kErrorSuccess;
}
