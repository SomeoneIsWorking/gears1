#pragma once

#include <cstddef>
#include <cstdint>

#include "guest_address.h"

namespace gears
{

// The retained native subsystems use absolute 32-bit Xenon addresses. This
// temporary owner preserves their proven aliasing behavior until x360port
// supplies Xenia Memory access; it owns no guest execution or function table.
class GuestMemory
{
  public:
    bool Reserve();
    void Release();

    uint8_t *Base() const { return base_; }

    // The extent of the mapping, needed by the fault reporter to tell a guest
    // address apart from a host pointer at the moment of a crash.
    size_t ReservedSize() const { return reservedSize_; }

    // Backs a guest address range with committed pages. Pages that have never
    // been handed out are already zero; a range whose address space is being
    // recycled still holds the previous tenant's bytes, so the caller that
    // knows which is which has to call Zero -- see GuestHeap::Allocate.
    bool Commit(uint32_t address, uint32_t size);

    // Zero-fills a committed guest range. NtAllocateVirtualMemory promises
    // zeroed pages on commit and the title's RtlHeap relies on it: it reads the
    // block header at the start of a freshly committed segment extension and
    // treats a non-zero one as a live heap block.
    void Zero(uint32_t address, uint32_t size);

    // Maps one block of real memory into every guest window that aliases
    // physical RAM, so a write through one view is visible through the others.
    bool MapPhysicalAliases();

    // Whether a HOST pointer lies inside this reservation.
    bool Contains(const void *host) const;

    // The offsets from Base() at which the same physical bytes appear. A
    // write through any one of them lands in all of them, so an observer that
    // must not miss a write -- page dirty tracking, for one -- has to consult
    // every window, not just the one it reads through.
    static constexpr size_t kAliasCount = 4;
    uint64_t AliasOffset(size_t i) const;
    // Mask extracting the physical offset from a guest address: guest code
    // converts between windows by clearing the top three bits.
    static constexpr uint32_t kAliasMask = kGuestPhysicalAddressMask;

    template <typename T> T *Translate(uint32_t address) const
    {
        return reinterpret_cast<T *>(base_ + address);
    }

  private:
    uint8_t *base_{};
    size_t reservedSize_{};
    int physicalFd_{-1};
};

// Read-only walk of the GPU command stream, reporting packets that mention
// `watchAddress`. Executes nothing; see pm4_trace.cpp.
void TraceCommandStream(uint32_t ringBase, uint32_t ringWords, uint32_t watchAddress);

// The process-wide guest memory. There is exactly one guest address space, and
// guest threads created later need to reach it without threading a reference
// through every kernel import.
GuestMemory &Memory();
void SetMemory(GuestMemory &memory);

} // namespace gears
