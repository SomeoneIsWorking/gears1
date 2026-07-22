#pragma once

#include <cstdint>
#include <map>
#include <mutex>

#include "guest_memory.h"
#include "kernel_status.h"

namespace gears
{

// Xbox 360 allocation flags, as passed to NtAllocateVirtualMemory.
constexpr uint32_t kMemCommit = 0x00001000;
constexpr uint32_t kMemReserve = 0x00002000;
constexpr uint32_t kMemLargePages = 0x20000000;

// A page-granular allocator over one span of the guest address space.
//
// The console splits guest memory into ranges with different page sizes; this
// covers the 64 KiB-page virtual range the title heap lives in.
//
// Free space is held as an address-ordered list of non-overlapping,
// non-adjacent regions; a free coalesces with its neighbours on insertion, so
// a repeated allocate/free cycle of the same shape reuses the same address
// space instead of walking forwards for ever. Allocation is first fit, which
// is enough for a title that allocates in a handful of recurring sizes; there
// is nothing clever here on purpose.
class GuestHeap
{
public:
    GuestHeap(GuestMemory& memory, uint32_t base, uint32_t size)
        : memory_(memory), base_(base), size_(size)
    {
        free_[base] = size;
        freeBytes_ = size;
    }

    // Returns the guest base address, or 0 if the request cannot be satisfied.
    // On success `size` is updated to the page-rounded size actually reserved.
    // `alignment` of 0 means "use the page size implied by allocationType".
    uint32_t Allocate(uint32_t requestedBase, uint32_t& size, uint32_t allocationType,
        uint32_t alignment = 0);

    // Releases a region previously returned by Allocate, making its address
    // space available again. The pages stay committed -- see the comment in
    // Free's implementation.
    bool Free(uint32_t address);

    uint32_t Size() const { return size_; }

    struct Usage
    {
        uint32_t allocated;   // bytes currently handed out
        uint32_t peak;        // high-water mark of `allocated`
        uint32_t free;        // total free bytes
        uint32_t largestFree; // biggest single free region
        uint32_t freeBlocks;  // how fragmented the free list is
        uint32_t regions;     // live allocations
    };
    Usage GetUsage() const;

    // Total free bytes. This is a real total now that frees are recycled, but
    // a request also needs one CONTIGUOUS run, so a large Available() is not on
    // its own a promise that a large allocation succeeds -- GetUsage().largestFree
    // is the number that answers that.
    uint32_t Available() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return freeBytes_;
    }

private:
    // All of these require mutex_ to be held.
    void InsertFree(uint32_t address, uint32_t size);
    void RemoveFreeRange(uint32_t address, uint32_t size);
    void NoteUsage();

    // Zero-fills the part of a newly claimed range whose address space has been
    // handed out before, and moves the high-water mark. Address space above the
    // mark has never been given to anyone, so its pages are still the zeroes
    // mmap gave us and memsetting them would only fault them in for nothing.
    // Must be called BEFORE the range is recorded, and only for space that is
    // transitioning free -> allocated: a re-commit inside a live region must
    // not lose the owner's data.
    void ZeroClaimed(uint32_t address, uint32_t size);

    GuestMemory& memory_;
    uint32_t base_;
    uint32_t size_;
    std::map<uint32_t, uint32_t> regions_; // live allocations: base -> size
    std::map<uint32_t, uint32_t> free_;    // free space: base -> size, coalesced
    uint32_t freeBytes_{};
    uint32_t allocatedBytes_{};
    uint32_t peakAllocated_{};
    uint32_t reportedPeak_{}; // last peak reported to the log
    uint32_t everAllocatedEnd_{}; // highest end ever handed out; above it, pages are still zero

    // Allocate/Free are reached from NtAllocateVirtualMemory and friends, which
    // guest threads call concurrently -- on the console these are kernel
    // syscalls and the kernel serialises them. Without this lock the std::map
    // corrupts under load (observed: spurious "free of unknown address"
    // warnings followed by a SIGSEGV inside the rb-tree erase).
    mutable std::mutex mutex_;
};

GuestHeap& TitleHeap();

// Physical memory, which the console maps near the top of the address space and
// the graphics path allocates from. Kept separate from the title heap because
// the guest distinguishes the two and passes physical addresses to the GPU.
GuestHeap& PhysicalHeap();

void InitialiseHeaps(GuestMemory& memory);

} // namespace gears
