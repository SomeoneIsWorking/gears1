#pragma once

#include <cstddef>
#include <cstdint>

namespace gears
{

enum class GuestWriteWatchOwner : uint8_t
{
    kQueue,
    kDrawPacket,
};

struct GuestWriteWatchStats
{
    bool armed = false;
    size_t aliasPages = 0;
    uint64_t targetWrites = 0;
    uint64_t otherPageWrites = 0;
};

struct DrawPacketWatchSelector
{
    bool frameSelected = false;
    bool done = false;
    uint32_t frameSequence = 0;
    uint32_t ordinalSeen = 0;

    [[nodiscard]] bool Observe(uint32_t swapSequence, int depth, uint32_t afterSwap,
                               uint32_t ordinal);
};

constexpr bool GuestWriteWatchContains(uintptr_t target, size_t targetBytes, uintptr_t fault)
{
    return targetBytes != 0 && fault >= target && fault - target < targetBytes;
}

// Arms one process-wide guest-memory write watch. Only one owner may hold the
// signal/mprotect facility at a time; a conflicting request refuses loudly.
// The page is reopened after each fault and protected again after the faulting
// store is single-stepped, so unrelated stores on the same page remain visible
// without being mistaken for writes to the exact target.
bool ArmGuestWriteWatch(GuestWriteWatchOwner owner, uint32_t guestAddress,
                        uint64_t targetSampleLimit);

// Publishes the captured host instruction offsets. With rearm=true the counts
// are per-report interval. A one-shot watch remains armed across empty reports
// and disarms only after it catches the requested target.
bool ReportGuestWriteWatch(GuestWriteWatchOwner owner, bool rearm);
GuestWriteWatchStats CurrentGuestWriteWatchStats(GuestWriteWatchOwner owner);

// Command-processor integration kept here so the legacy protocol owner needs
// only one observation call and does not acquire diagnostic policy/state.
void MaybeArmDrawPacketWriteWatch(uint32_t sourceBase, uint32_t sourceIndex, int depth,
                                  uint32_t swapSequence);
void ReportDrawPacketWriteWatch();

} // namespace gears
