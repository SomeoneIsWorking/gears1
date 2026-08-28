#include "gpu_ticket_wait.h"

#include "guest_address.h"

namespace gears
{

namespace
{

GpuPacketMemoryChangeTracker g_packetMemoryChanges;

[[nodiscard]] std::uint32_t NormalizeGpuAddress(std::uint32_t address)
{
    return address & kGuestPhysicalAddressMask & ~std::uint32_t{3};
}

} // namespace

std::chrono::milliseconds RemainingGpuTicketWait(std::uint32_t currentTick,
                                                 std::uint32_t lastProgressTick)
{
    const std::uint32_t elapsed = currentTick - lastProgressTick;
    if (elapsed >= kGpuTicketHangTimeoutMilliseconds)
        return std::chrono::milliseconds::zero();
    return std::chrono::milliseconds{kGpuTicketHangTimeoutMilliseconds - elapsed};
}

GpuPacketMemoryObservation GpuPacketMemoryChangeTracker::Observe(std::uint32_t address)
{
    std::lock_guard lock(mutex_);
    return {.address = address, .generation = generations_[address]};
}

bool GpuPacketMemoryChangeTracker::WaitUntil(GpuPacketMemoryObservation observation,
                                             std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock(mutex_);
    return changed_.wait_until(
        lock, deadline, [this, observation]
        { return generations_[observation.address] != observation.generation; });
}

void GpuPacketMemoryChangeTracker::Notify(std::uint32_t address)
{
    {
        std::lock_guard lock(mutex_);
        ++generations_[address];
    }
    changed_.notify_all();
}

GpuPacketMemoryObservation ObserveGpuPacketMemory(std::uint32_t guestAddress)
{
    return g_packetMemoryChanges.Observe(NormalizeGpuAddress(guestAddress));
}

bool WaitForGpuPacketMemoryChange(GpuPacketMemoryObservation observation,
                                  std::chrono::steady_clock::time_point deadline)
{
    return g_packetMemoryChanges.WaitUntil(observation, deadline);
}

void NotifyGpuPacketMemoryChanged(std::uint32_t guestAddress)
{
    g_packetMemoryChanges.Notify(NormalizeGpuAddress(guestAddress));
}

} // namespace gears
