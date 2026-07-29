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
        // The link register names the caller, which is the only cheap way to
        // find out WHY a title wants a size the console could never satisfy
        // (catalog #45: 2.6 GB requested on the save path).
        lucent::warn("kernel", "NtAllocateVirtualMemory failed, called from {:#x}:"
            " base={:#x} size={:#x} type={:#x} protect={:#x}", uint32_t(ctx.lr),
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
//
// FreeType matters now that the heap recycles address space. MEM_DECOMMIT
// drops the pages but KEEPS the reservation, so the guest still owns the
// address range and handing it to another allocation would corrupt it; only
// MEM_RELEASE gives the range back. The pages themselves stay committed either
// way -- see GuestHeap::Free.
void __imp__NtFreeVirtualMemory(PPCContext& __restrict ctx, uint8_t* base)
{
    constexpr uint32_t kMemDecommit = 0x4000;
    constexpr uint32_t kMemRelease = 0x8000;

    const uint32_t baseAddressPtr = ctx.r3.u32;
    const uint32_t freeType = ctx.r5.u32;
    if (baseAddressPtr == 0)
    {
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    const uint32_t address = GuestLoad32(base, baseAddressPtr);

    // A NULL base is answered here, before any heap is consulted. It is not a
    // runtime bug: the title's own VirtualFree wrapper (sub_82612658) forwards
    // its argument with no NULL check, and D3D's resource destructor
    // (sub_82214C70) calls it on a resource whose data pointer it never filled
    // in, so a free of 0 is normal traffic from the guest. Sending it to the
    // allocator instead is what produced the "free of unknown address 0x0"
    // warning on every run. The console answers the same way: Xenia
    // xboxkrnl_memory.cc NtFreeVirtualMemory_entry returns
    // X_STATUS_MEMORY_NOT_ALLOCATED for a zero base without touching a heap,
    // and the wrapper turns that negative status into a FALSE return.
    if (address == 0)
    {
        ctx.r3.u64 = gears::kStatusMemoryNotAllocated;
        return;
    }

    // Which heap owns a pointer is decided by its ADDRESS, not by the export
    // the guest happened to call -- the D3D destructor above picks between the
    // physical and virtual free wrappers from a flag on the resource. Xenia
    // looks the heap up the same way and REFUSES this call when the heap is
    // not the guest-virtual one, rather than releasing from it.
    if (gears::HeapForAddress(address) != &gears::TitleHeap())
    {
        lucent::warn("kernel", "NtFreeVirtualMemory({:#x}) from {:#x}: not a title-heap address",
            address, uint32_t(ctx.lr));
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    if ((freeType & kMemRelease) != 0 || freeType == 0)
    {
        // A FreeType of 0 is not a documented value; treating it as a release
        // matches what this import did before FreeType was honoured at all, so
        // an unexpected caller does not silently start leaking.
        //
        // A failed release is reported rather than swallowed: this used to
        // return SUCCESS unconditionally, which told the title's VirtualFree
        // that a free it never performed had worked.
        if (!gears::TitleHeap().Free(address))
        {
            ctx.r3.u64 = gears::kStatusUnsuccessful;
            return;
        }
    }
    else
    {
        lucent::debug("kernel", "NtFreeVirtualMemory({:#x}, type {:#x}) decommit only -- reservation kept",
            address, freeType);
        if ((freeType & kMemDecommit) == 0)
            lucent::warn("kernel", "NtFreeVirtualMemory({:#x}): unexpected FreeType {:#x}",
                address, freeType);
    }
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

    // Honour the alignment the caller asked for rather than forcing 64 KiB
    // pages: the title packs its own structures inside these blocks and lays
    // them out from the granularity it requested.
    const uint32_t address = gears::PhysicalHeap().Allocate(
        0, size, gears::kMemCommit, alignment);

    if (address == 0)
        lucent::warn("kernel", "MmAllocatePhysicalMemoryEx({:#x}, align {:#x}) failed", size, alignment);

    ctx.r3.u64 = address;
}

void __imp__MmAllocatePhysicalMemory(PPCContext& __restrict ctx, uint8_t*)
{
    uint32_t size = ctx.r4.u32;
    ctx.r3.u64 = gears::PhysicalHeap().Allocate(0, size, gears::kMemCommit | gears::kMemLargePages);
}

// VOID MmFreePhysicalMemory(ULONG Type, PVOID BaseAddress)
//
// The argument order is the console's: Xenia's MmFreePhysicalMemory_entry takes
// (type, base_address), and the title's XPhysicalFree wrapper (sub_82612758)
// moves its pointer argument into r4 and sets r3 to 0 before tail-calling here.
void __imp__MmFreePhysicalMemory(PPCContext& __restrict ctx, uint8_t*)
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
    gears::GuestHeap* heap = gears::HeapForAddress(address);
    if (heap != &gears::PhysicalHeap())
    {
        lucent::warn("kernel", "MmFreePhysicalMemory({:#x}) from {:#x}: not a physical-window address",
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
void __imp__MmGetPhysicalAddress(PPCContext& __restrict ctx, uint8_t*)
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
void __imp__MmSetAddressProtect(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("kernel", "MmSetAddressProtect({:#x}, {:#x}, protect={:#x}) -- not enforced",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}

void __imp__MmQueryAddressProtect(PPCContext& __restrict ctx, uint8_t*)
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
void __imp__MmQueryStatistics(PPCContext& __restrict ctx, uint8_t* base)
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

    const uint32_t length = ByteSwap(*reinterpret_cast<uint32_t*>(base + out));
    if (length != kStatisticsSize)
    {
        lucent::warn("kernel", "MmQueryStatistics: unexpected struct length {:#x}", length);
        ctx.r3.u64 = gears::kStatusInvalidParameter;
        return;
    }

    auto store = [&](uint32_t offset, uint32_t value) {
        *reinterpret_cast<uint32_t*>(base + out + offset) = ByteSwap(value);
    };

    for (uint32_t offset = 4; offset < kStatisticsSize; offset += 4)
        store(offset, 0);

    const uint32_t titleAvailable = gears::TitleHeap().Available() / kPageSize;

    store(0x04, kTotalPhysicalPages);            // total physical pages
    store(0x0C, titleAvailable);                 // title available pages
    store(0x10, gears::TitleHeap().Size());      // title total virtual bytes
    store(0x18, gears::PhysicalHeap().Size() / kPageSize); // title physical pages
    store(0x38, titleAvailable);                 // system available pages

    lucent::debug("kernel", "MmQueryStatistics -> {} of {} pages available",
        titleAvailable, kTotalPhysicalPages);
    ctx.r3.u64 = gears::kStatusSuccess;
}
