#include "guest_heap.h"

#include <lucent/log.h>

namespace gears
{

namespace
{
uint32_t PageSizeFor(uint32_t allocationType)
{
    // The console distinguishes 4 KiB and 64 KiB page ranges; the flag tells us
    // which the caller expects, and the alignment of what we hand back has to
    // match or the game's own suballocators will disagree with us.
    return (allocationType & kMemLargePages) != 0 ? 0x10000u : 0x1000u;
}

uint32_t RoundUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

GuestHeap* g_titleHeap = nullptr;
} // namespace

uint32_t GuestHeap::Allocate(uint32_t requestedBase, uint32_t& size, uint32_t allocationType)
{
    const uint32_t pageSize = PageSizeFor(allocationType);
    const uint32_t roundedSize = RoundUp(size, pageSize);
    if (roundedSize == 0)
        return 0;

    uint32_t address;
    if (requestedBase != 0)
    {
        // Fixed allocation. Committing an already-reserved range is legitimate
        // and common, so this is not treated as a conflict.
        address = requestedBase & ~(pageSize - 1);
        if (address < base_ || uint64_t(address) + roundedSize > uint64_t(base_) + size_)
        {
            lucent::error("heap", "fixed allocation {:#x}+{:#x} is outside the heap {:#x}+{:#x}",
                address, roundedSize, base_, size_);
            return 0;
        }
    }
    else
    {
        address = RoundUp(cursor_, pageSize);
        if (uint64_t(address) + roundedSize > uint64_t(base_) + size_)
        {
            lucent::error("heap", "out of guest heap: wanted {:#x} bytes, {:#x} left",
                roundedSize, uint64_t(base_) + size_ - address);
            return 0;
        }
        cursor_ = address + roundedSize;
    }

    if (!memory_.Commit(address, roundedSize))
        return 0;

    regions_[address] = roundedSize;
    size = roundedSize;

    lucent::debug("heap", "allocated {:#x}+{:#x} (type {:#x}, {} KiB pages)",
        address, roundedSize, allocationType, pageSize / 1024);
    return address;
}

bool GuestHeap::Free(uint32_t address)
{
    auto it = regions_.find(address);
    if (it == regions_.end())
    {
        lucent::warn("heap", "free of unknown address {:#x}", address);
        return false;
    }

    // The pages stay committed: the guest may still hold aliases into them and
    // decommitting would turn a use-after-free into a host crash far from its
    // cause. Address space is not the scarce resource here.
    lucent::debug("heap", "freed {:#x}+{:#x}", address, it->second);
    regions_.erase(it);
    return true;
}

GuestHeap& TitleHeap()
{
    return *g_titleHeap;
}

void InitialiseTitleHeap(GuestMemory& memory)
{
    // The 64 KiB-page virtual range on the console. Kept clear of the image
    // (0x82000000), the stack and the import-variable storage.
    static GuestHeap heap(memory, 0x40000000, 0x20000000);
    g_titleHeap = &heap;
    lucent::info("heap", "title heap at 0x40000000, 512 MiB");
}

} // namespace gears
