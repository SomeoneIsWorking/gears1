// Recovered implementations of the kernel's physical-memory and address-query
// imports. The virtual-memory exports they used to sit beside now run in
// x360port over Xenia's own heaps; these remain until their owner is built.
#include "import_stub.h"

#include "byte_order.h"
#include <lucent/log.h>

#include "guest_heap.h"
#include "guest_memory.h"

// PVOID MmAllocatePhysicalMemoryEx(ULONG Flags, SIZE_T Size, ULONG Protect,
//                                  ULONG MinAddress, ULONG MaxAddress,
//                                  ULONG Alignment)
//
// Returns a physical address, which the guest passes on to the GPU, so it has
// to come from the physical window rather than the title heap.
void __imp__MmAllocatePhysicalMemoryEx(PPCContext &__restrict ctx, uint8_t *)
{
    uint32_t size = ctx.r4.u32;
    const uint32_t alignment = ctx.r8.u32;

    // Honour the alignment the caller asked for rather than forcing 64 KiB
    // pages: the title packs its own structures inside these blocks and lays
    // them out from the granularity it requested.
    const uint32_t address = gears::PhysicalHeap().Allocate(0, size, gears::kMemCommit, alignment);

    if (address == 0)
        lucent::warn("kernel", "MmAllocatePhysicalMemoryEx({:#x}, align {:#x}) failed", size,
                     alignment);

    ctx.r3.u64 = address;
}

void __imp__MmAllocatePhysicalMemory(PPCContext &__restrict ctx, uint8_t *)
{
    uint32_t size = ctx.r4.u32;
    ctx.r3.u64 = gears::PhysicalHeap().Allocate(0, size, gears::kMemCommit | gears::kMemLargePages);
}

// VOID MmFreePhysicalMemory(ULONG Type, PVOID BaseAddress)
//
// The argument order is the console's: Xenia's MmFreePhysicalMemory_entry takes
// (type, base_address), and the title's XPhysicalFree wrapper (sub_82612758)
// moves its pointer argument into r4 and sets r3 to 0 before tail-calling here.
void __imp__MmFreePhysicalMemory(PPCContext &__restrict ctx, uint8_t *)
{
    const uint32_t address = ctx.r4.u32;

    // XPhysicalFree has no NULL guard, and D3D's resource destructor
    // (sub_82214C70) frees a resource whose data pointer is still 0, so this
    // arrives on a normal run. Nothing was ever allocated at 0, so there is
    // nothing to release and no heap to consult -- Xenia reaches the same
    // outcome by looking the heap up by address, landing outside the physical
    // window, and failing the release there.
    if (address == 0)
    {
        lucent::debug("kernel", "MmFreePhysicalMemory(0) from {:#x} -- nothing allocated there",
                      uint32_t(ctx.lr));
        return;
    }

    // Routed by address rather than assumed physical: a title-heap pointer
    // arriving here is a real bug and has to stay visible, not be handed to the
    // physical heap where it can only report as unknown.
    gears::GuestHeap *heap = gears::HeapForAddress(address);
    if (heap != &gears::PhysicalHeap())
    {
        lucent::warn("kernel",
                     "MmFreePhysicalMemory({:#x}) from {:#x}: not a physical-window address",
                     address, uint32_t(ctx.lr));
        return;
    }

    heap->Free(address);
}

// PVOID MmGetPhysicalAddress(PVOID Address)
//
// The console maps one bank of physical memory into several virtual windows --
// 0xA0000000, 0xC0000000 and 0xE0000000 differ only in caching behaviour -- and
// a physical address is the offset WITHIN that bank. Returning the virtual
// address unchanged, which this used to do, is only correct for a pointer that
// was already physical.
//
// It is not a cosmetic difference. A title that takes the physical address of a
// buffer in the 0xA0000000 window and subtracts it from another address gets a
// number about 2.6 GB too large, because the window base is still in it -- and
// an unsigned subtraction that should have been small underflows instead
// (catalog #45: 0x4F800000 - 0xB0800000 is exactly the 0x9F000000 that failed).
//
// Xenia does the same subtraction (memory.cc PhysicalHeap::GetPhysicalAddress),
// including the 0x1000 the 0xE0000000 window is offset by.
void __imp__MmGetPhysicalAddress(PPCContext &__restrict ctx, uint8_t *)
{
    const uint32_t address = ctx.r3.u32;
    uint32_t physical = address;
    if (address >= 0xE0000000)
        physical = address - 0xE0000000 + 0x1000;
    else if (address >= 0xC0000000)
        physical = address - 0xC0000000;
    else if (address >= 0xA0000000)
        physical = address - 0xA0000000;
    // Anything else is either already a physical address or not in a physical
    // window at all, and inventing a translation for it would be worse than
    // passing it through.

    lucent::debug("kernel", "MmGetPhysicalAddress({:#x}) -> {:#x}", address, physical);
    ctx.r3.u64 = physical;
}

// VOID MmSetAddressProtect(PVOID Address, ULONG Size, ULONG Protect)
//
// Page protection is not modelled: the guest heap commits everything readable
// and writable, and enforcing the guest's requested protections would only turn
// its own valid accesses into host faults. Recorded so a later access-violation
// investigation can see what the title intended.
void __imp__MmSetAddressProtect(PPCContext &__restrict ctx, uint8_t *)
{
    lucent::debug("kernel", "MmSetAddressProtect({:#x}, {:#x}, protect={:#x}) -- not enforced",
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}

void __imp__MmQueryAddressProtect(PPCContext &__restrict ctx, uint8_t *)
{
    ctx.r3.u64 = 0x04; // PAGE_READWRITE, matching how the heap is committed
}

// NTSTATUS MmQueryStatistics(PMM_STATISTICS out)
//
// The caller sets the leading Length field and the kernel refuses a struct it
// does not recognise, so that check is honoured rather than blindly filling.
//
// The console's own numbers are reported where they are properties of the
// hardware (512 MiB of physical memory in 4 KiB pages) and the runtime's real
// heap state where they are properties of this process. Fields the runtime
// genuinely does not track are left zero rather than filled with plausible
// numbers, which would be indistinguishable from real ones to a title sizing
// its caches off them.
void __imp__MmQueryStatistics(PPCContext &__restrict ctx, uint8_t *base)
{
    constexpr uint32_t kStatisticsSize = 0x68;
    constexpr uint32_t kPageSize = 4096;
    constexpr uint32_t kTotalPhysicalPages = 512u * 1024u * 1024u / kPageSize;

    const uint32_t out = ctx.r3.u32;
    if (out == 0)
    {
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    const uint32_t length = ByteSwap(*reinterpret_cast<uint32_t *>(base + out));
    if (length != kStatisticsSize)
    {
        lucent::warn("kernel", "MmQueryStatistics: unexpected struct length {:#x}", length);
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    auto store = [&](uint32_t offset, uint32_t value)
    { *reinterpret_cast<uint32_t *>(base + out + offset) = ByteSwap(value); };

    for (uint32_t offset = 4; offset < kStatisticsSize; offset += 4)
        store(offset, 0);

    const uint32_t titleAvailable = gears::TitleHeap().Available() / kPageSize;

    store(0x04, kTotalPhysicalPages);                      // total physical pages
    store(0x0C, titleAvailable);                           // title available pages
    store(0x10, gears::TitleHeap().Size());                // title total virtual bytes
    store(0x18, gears::PhysicalHeap().Size() / kPageSize); // title physical pages
    store(0x38, titleAvailable);                           // system available pages

    lucent::debug("kernel", "MmQueryStatistics -> {} of {} pages available", titleAvailable,
                  kTotalPhysicalPages);
    ctx.r3.u64 = gears::kStatusSuccess;
}
