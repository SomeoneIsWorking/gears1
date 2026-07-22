#include "guest_heap.h"

#include <iterator>

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
GuestHeap* g_physicalHeap = nullptr;
} // namespace

// Adds [address, address+size) to the free list, merging it with an adjacent
// free region on either side. Without the merge, a title that frees a block and
// then asks for a slightly larger one can never reuse the space, and the heap
// fragments its way back to exhaustion.
void GuestHeap::InsertFree(uint32_t address, uint32_t size)
{
    if (size == 0)
        return;

    auto next = free_.lower_bound(address);
    if (next != free_.begin())
    {
        auto prev = std::prev(next);
        const uint64_t prevEnd = uint64_t(prev->first) + prev->second;
        if (prevEnd > address)
        {
            // Two live regions overlapped, or something was freed twice. Either
            // is a bookkeeping bug in the runtime, and silently merging would
            // hand the same bytes out to two owners.
            lucent::error("heap", "free of {:#x}+{:#x} overlaps free region {:#x}+{:#x}",
                address, size, prev->first, prev->second);
            return;
        }
        if (prevEnd == address)
        {
            address = prev->first;
            size += prev->second;
            freeBytes_ -= prev->second;
            free_.erase(prev);
        }
    }

    if (next != free_.end())
    {
        if (uint64_t(address) + size > next->first)
        {
            lucent::error("heap", "free of {:#x}+{:#x} overlaps free region {:#x}+{:#x}",
                address, size, next->first, next->second);
            return;
        }
        if (uint64_t(address) + size == next->first)
        {
            size += next->second;
            freeBytes_ -= next->second;
            free_.erase(next);
        }
    }

    free_[address] = size;
    freeBytes_ += size;
}

// Takes [address, address+size) out of the free list, clipping every free
// region that overlaps it. Parts of the range that are already allocated are
// left alone: a fixed allocation that re-commits a range the guest already
// reserved is legitimate, and only the still-free part has to be claimed.
void GuestHeap::RemoveFreeRange(uint32_t address, uint32_t size)
{
    const uint64_t end = uint64_t(address) + size;

    auto it = free_.lower_bound(address);
    if (it != free_.begin())
    {
        auto prev = std::prev(it);
        if (uint64_t(prev->first) + prev->second > address)
            it = prev;
    }

    while (it != free_.end() && it->first < end)
    {
        const uint32_t blockStart = it->first;
        const uint64_t blockEnd = uint64_t(blockStart) + it->second;
        const uint32_t overlapStart = blockStart > address ? blockStart : address;
        const uint64_t overlapEnd = blockEnd < end ? blockEnd : end;
        if (overlapStart >= overlapEnd)
        {
            ++it;
            continue;
        }

        freeBytes_ -= it->second;
        it = free_.erase(it);

        if (blockStart < overlapStart)
        {
            free_[blockStart] = uint32_t(overlapStart - blockStart);
            freeBytes_ += uint32_t(overlapStart - blockStart);
        }
        if (overlapEnd < blockEnd)
        {
            free_[uint32_t(overlapEnd)] = uint32_t(blockEnd - overlapEnd);
            freeBytes_ += uint32_t(blockEnd - overlapEnd);
            it = free_.find(uint32_t(overlapEnd));
            ++it;
        }
    }
}

// Reports the high-water mark as it moves, in 16 MiB steps. A heap that is
// genuinely reusing its space plateaus here; one that leaks keeps printing.
void GuestHeap::NoteUsage()
{
    constexpr uint32_t kStep = 16u * 1024 * 1024;
    if (allocatedBytes_ <= peakAllocated_)
        return;
    peakAllocated_ = allocatedBytes_;
    if (peakAllocated_ < reportedPeak_ + kStep)
        return;
    reportedPeak_ = peakAllocated_ - (peakAllocated_ % kStep);
    lucent::info("heap", "{:#x}: peak use {} MiB of {} MiB ({} live regions, {} free blocks)",
        base_, peakAllocated_ / (1024 * 1024), size_ / (1024 * 1024),
        regions_.size(), free_.size());
}

uint32_t GuestHeap::Allocate(uint32_t requestedBase, uint32_t& size, uint32_t allocationType,
    uint32_t alignment)
{
    uint32_t pageSize = PageSizeFor(allocationType);
    if (alignment > pageSize)
        pageSize = alignment;
    const uint32_t roundedSize = RoundUp(size, pageSize);
    if (roundedSize == 0)
        return 0;

    std::lock_guard<std::mutex> lock(mutex_);

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

        // If the range already lies inside a live region, this is a re-commit
        // of something the guest reserved earlier. It must NOT get a second
        // bookkeeping entry: the guest frees the reservation by its own base,
        // and a leftover inner entry would later release address space that is
        // already in use.
        auto owner = regions_.upper_bound(address);
        if (owner != regions_.begin())
        {
            --owner;
            const uint64_t ownerEnd = uint64_t(owner->first) + owner->second;
            if (ownerEnd > address)
            {
                if (uint64_t(address) + roundedSize > ownerEnd)
                {
                    // Grows the existing reservation upwards.
                    RemoveFreeRange(uint32_t(ownerEnd), uint32_t(uint64_t(address) + roundedSize - ownerEnd));
                    allocatedBytes_ += uint32_t(uint64_t(address) + roundedSize - ownerEnd);
                    owner->second = uint32_t(uint64_t(address) + roundedSize - owner->first);
                    NoteUsage();
                }
                if (!memory_.Commit(address, roundedSize))
                    return 0;
                size = roundedSize;
                lucent::debug("heap", "re-committed {:#x}+{:#x} inside {:#x}+{:#x}",
                    address, roundedSize, owner->first, owner->second);
                return address;
            }
        }

        // The range starts in free space, but it may still run into a region
        // that begins later. Creating an entry for it would give two owners the
        // same bytes, and whichever freed first would put live memory back on
        // the free list.
        auto conflict = regions_.lower_bound(address);
        if (conflict != regions_.end() && conflict->first < uint64_t(address) + roundedSize)
        {
            lucent::error("heap", "fixed allocation {:#x}+{:#x} overlaps live region {:#x}+{:#x}",
                address, roundedSize, conflict->first, conflict->second);
            if (!memory_.Commit(address, roundedSize))
                return 0;
            size = roundedSize;
            return address;
        }

        RemoveFreeRange(address, roundedSize);
    }
    else
    {
        // First fit over the free list, honouring the alignment inside the
        // block rather than assuming a free block starts aligned -- a block
        // formed by coalescing 4 KiB-page frees can start anywhere.
        address = 0;
        for (auto it = free_.begin(); it != free_.end(); ++it)
        {
            const uint32_t start = RoundUp(it->first, pageSize);
            const uint64_t blockEnd = uint64_t(it->first) + it->second;
            if (uint64_t(start) + roundedSize > blockEnd)
                continue;

            const uint32_t blockStart = it->first;
            const uint32_t blockSize = it->second;
            freeBytes_ -= blockSize;
            free_.erase(it);
            if (blockStart < start)
            {
                free_[blockStart] = start - blockStart;
                freeBytes_ += start - blockStart;
            }
            if (uint64_t(start) + roundedSize < blockEnd)
            {
                const uint32_t tail = uint32_t(start) + roundedSize;
                free_[tail] = uint32_t(blockEnd - tail);
                freeBytes_ += uint32_t(blockEnd - tail);
            }
            address = start;
            break;
        }

        if (address == 0)
        {
            uint32_t largest = 0;
            for (const auto& [blockStart, blockSize] : free_)
            {
                const uint32_t usable = RoundUp(blockStart, pageSize) >= blockStart + blockSize
                    ? 0 : blockSize - (RoundUp(blockStart, pageSize) - blockStart);
                if (usable > largest)
                    largest = usable;
            }
            lucent::error("heap",
                "out of guest heap {:#x}: wanted {:#x} bytes, {:#x} free in {} blocks, largest usable {:#x}",
                base_, roundedSize, freeBytes_, free_.size(), largest);
            return 0;
        }
    }

    if (!memory_.Commit(address, roundedSize))
    {
        // Put the space back rather than losing it to a failed commit. Only the
        // non-fixed path may do this: a fixed request can have carved part of
        // its range out of a reservation the guest still owns, and returning
        // the whole range would free memory that is still live.
        if (requestedBase == 0)
            InsertFree(address, roundedSize);
        return 0;
    }

    regions_[address] = roundedSize;
    allocatedBytes_ += roundedSize;
    NoteUsage();
    size = roundedSize;

    // The alignment is recorded because a replay of this trace cannot recover
    // it from the address: a working allocator does not reproduce the recorded
    // addresses, and inferring alignment from them over-aligns wildly.
    lucent::debug("heap", "allocated {:#x}+{:#x} (type {:#x}, align {:#x}, {} KiB pages)",
        address, roundedSize, allocationType, alignment, pageSize / 1024);
    return address;
}

bool GuestHeap::Free(uint32_t address)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = regions_.find(address);
    if (it == regions_.end())
    {
        lucent::warn("heap", "free of unknown address {:#x}", address);
        return false;
    }

    const uint32_t regionSize = it->second;

    // The pages stay COMMITTED: the guest may still hold aliases into them and
    // decommitting would turn a use-after-free into a host fault far from its
    // cause. Only the address space goes back to the free list, so a stale
    // guest pointer still reads and writes real memory -- it just may now be
    // memory somebody else owns, which is guest misbehaviour and shows up as
    // such rather than as a segfault in the runtime.
    lucent::debug("heap", "freed {:#x}+{:#x}", address, regionSize);
    regions_.erase(it);
    allocatedBytes_ -= regionSize;
    InsertFree(address, regionSize);
    return true;
}

GuestHeap::Usage GuestHeap::GetUsage() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t largest = 0;
    for (const auto& [blockStart, blockSize] : free_)
        if (blockSize > largest)
            largest = blockSize;
    return Usage{ allocatedBytes_, peakAllocated_, freeBytes_, largest,
        uint32_t(free_.size()), uint32_t(regions_.size()) };
}

GuestHeap& TitleHeap()
{
    return *g_titleHeap;
}

GuestHeap& PhysicalHeap()
{
    return *g_physicalHeap;
}

void InitialiseHeaps(GuestMemory& memory)
{
    // The 64 KiB-page virtual range on the console. Kept clear of the image
    // (0x82000000), the stack and the import-variable storage.
    static GuestHeap titleHeap(memory, 0x40000000, 0x20000000);
    g_titleHeap = &titleHeap;
    lucent::info("heap", "title heap at 0x40000000, 512 MiB");

    // The console's physical range. Addresses from here are what the guest
    // hands to the GPU, so they must come from this window and not the title
    // heap.
    static GuestHeap physicalHeap(memory, 0xA0000000, 0x20000000);
    g_physicalHeap = &physicalHeap;
    lucent::info("heap", "physical heap at 0xA0000000, 512 MiB");
}

} // namespace gears
