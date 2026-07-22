// Launch data: the block a title can be handed by whatever launched it (a
// dashboard, another title). Nothing launches this one with data, so the
// honest answer to every query is "none" -- which is a state the console
// itself produces on a normal boot from disc, not a stand-in for missing work.
#include "import_stub.h"

#include <lucent/log.h>

#include <byteswap.h>

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
