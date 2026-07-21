// Implementations of the Xbox 360 kernel's virtual-memory imports. Their names
// are listed in implemented_imports.h, which suppresses the generated stubs.
#include "import_stub.h"

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_heap.h"
#include "guest_memory.h"

namespace
{
uint32_t GuestLoad32(uint8_t* base, uint32_t address)
{
    return ByteSwap(*reinterpret_cast<uint32_t*>(base + address));
}

void GuestStore32(uint8_t* base, uint32_t address, uint32_t value)
{
    *reinterpret_cast<uint32_t*>(base + address) = ByteSwap(value);
}
} // namespace

// NTSTATUS NtAllocateVirtualMemory(PVOID* BaseAddress, SIZE_T* RegionSize,
//                                  ULONG AllocationType, ULONG Protect,
//                                  ULONG DebugMemory)
//
// BaseAddress and RegionSize are in/out: the caller may request a fixed address
// (or 0 to let the kernel choose) and always reads back the page-rounded size.
void __imp__NtAllocateVirtualMemory(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t baseAddressPtr = ctx.r3.u32;
    const uint32_t regionSizePtr = ctx.r4.u32;
    const uint32_t allocationType = ctx.r5.u32;
    const uint32_t protect = ctx.r6.u32;

    if (baseAddressPtr == 0 || regionSizePtr == 0)
    {
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    const uint32_t requestedBase = GuestLoad32(base, baseAddressPtr);
    uint32_t size = GuestLoad32(base, regionSizePtr);

    const uint32_t address = gears::TitleHeap().Allocate(requestedBase, size, allocationType);
    if (address == 0)
    {
        lucent::warn("kernel", "NtAllocateVirtualMemory failed: base={:#x} size={:#x} type={:#x} protect={:#x}",
            requestedBase, size, allocationType, protect);
        ctx.r3.u64 = gears::kStatusNoMemory;
        return;
    }

    GuestStore32(base, baseAddressPtr, address);
    GuestStore32(base, regionSizePtr, size);
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS NtFreeVirtualMemory(PVOID* BaseAddress, SIZE_T* RegionSize,
//                              ULONG FreeType, ULONG DebugMemory)
void __imp__NtFreeVirtualMemory(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t baseAddressPtr = ctx.r3.u32;
    if (baseAddressPtr == 0)
    {
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    gears::TitleHeap().Free(GuestLoad32(base, baseAddressPtr));
    ctx.r3.u64 = gears::kStatusSuccess;
}
