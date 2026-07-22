#pragma once

#include <cstdint>
#include <map>

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
// covers the 64 KiB-page virtual range the title heap lives in. It tracks
// reservations so a later free knows how big the region was, but deliberately
// does no coalescing or reuse beyond whole regions -- enough to be correct,
// with no cleverness to be wrong about.
class GuestHeap
{
public:
    GuestHeap(GuestMemory& memory, uint32_t base, uint32_t size)
        : memory_(memory), base_(base), size_(size), cursor_(base) {}

    // Returns the guest base address, or 0 if the request cannot be satisfied.
    // On success `size` is updated to the page-rounded size actually reserved.
    // `alignment` of 0 means "use the page size implied by allocationType".
    uint32_t Allocate(uint32_t requestedBase, uint32_t& size, uint32_t allocationType,
        uint32_t alignment = 0);
    bool Free(uint32_t address);

    uint32_t Size() const { return size_; }

    // Everything past the cursor. Freed regions are not recycled, so this is
    // what a further allocation can actually get rather than a bookkeeping
    // total that would over-promise.
    uint32_t Available() const { return uint32_t(uint64_t(base_) + size_ - cursor_); }

private:
    GuestMemory& memory_;
    uint32_t base_;
    uint32_t size_;
    uint32_t cursor_;
    std::map<uint32_t, uint32_t> regions_; // base -> size
};

GuestHeap& TitleHeap();

// Physical memory, which the console maps near the top of the address space and
// the graphics path allocates from. Kept separate from the title heap because
// the guest distinguishes the two and passes physical addresses to the GPU.
GuestHeap& PhysicalHeap();

void InitialiseHeaps(GuestMemory& memory);

} // namespace gears
