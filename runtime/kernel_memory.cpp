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

// PVOID MmAllocatePhysicalMemoryEx(ULONG Flags, SIZE_T Size, ULONG Protect,
//                                  ULONG MinAddress, ULONG MaxAddress,
//                                  ULONG Alignment)
//
// Returns a physical address, which the guest passes on to the GPU, so it has
// to come from the physical window rather than the title heap.
void __imp__MmAllocatePhysicalMemoryEx(PPCContext& __restrict ctx, uint8_t*)
{
    uint32_t size = ctx.r4.u32;
    const uint32_t alignment = ctx.r8.u32;

    const uint32_t address = gears::PhysicalHeap().Allocate(
        0, size, gears::kMemCommit | gears::kMemLargePages, alignment);

    if (address == 0)
        lucent::warn("kernel", "MmAllocatePhysicalMemoryEx({:#x}, align {:#x}) failed", size, alignment);

    ctx.r3.u64 = address;
}

void __imp__MmAllocatePhysicalMemory(PPCContext& __restrict ctx, uint8_t*)
{
    uint32_t size = ctx.r4.u32;
    ctx.r3.u64 = gears::PhysicalHeap().Allocate(0, size, gears::kMemCommit | gears::kMemLargePages);
}

void __imp__MmFreePhysicalMemory(PPCContext& __restrict ctx, uint8_t*)
{
    gears::PhysicalHeap().Free(ctx.r4.u32);
}

// The guest's virtual and physical views are the same buffer here, so a
// physical address is its own translation.
void __imp__MmGetPhysicalAddress(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = ctx.r3.u32;
}

// VOID MmSetAddressProtect(PVOID Address, ULONG Size, ULONG Protect)
//
// Page protection is not modelled: the guest heap commits everything readable
// and writable, and enforcing the guest's requested protections would only turn
// its own valid accesses into host faults. Recorded so a later access-violation
// investigation can see what the title intended.
void __imp__MmSetAddressProtect(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("kernel", "MmSetAddressProtect({:#x}, {:#x}, protect={:#x}) -- not enforced",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}

void __imp__MmQueryAddressProtect(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0x04; // PAGE_READWRITE, matching how the heap is committed
}

