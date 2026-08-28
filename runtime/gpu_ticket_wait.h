#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace gears
{

constexpr std::uint32_t kGpuTicketHangTimeoutMilliseconds = 5'000;

[[nodiscard]] std::chrono::milliseconds RemainingGpuTicketWait(std::uint32_t currentTick,
                                                               std::uint32_t lastProgressTick);

struct GpuPacketMemoryObservation
{
    std::uint32_t address = 0;
    std::uint64_t generation = 0;
};

// Address-keyed notification for guest memory written by the emulated GPU.
// Observe-before-read plus the generation predicate prevents a publication
// between the title's memory read and its host wait from being lost.
class GpuPacketMemoryChangeTracker
{
  public:
    [[nodiscard]] GpuPacketMemoryObservation Observe(std::uint32_t address);
    [[nodiscard]] bool WaitUntil(GpuPacketMemoryObservation observation,
                                 std::chrono::steady_clock::time_point deadline);
    void Notify(std::uint32_t address);

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::unordered_map<std::uint32_t, std::uint64_t> generations_;
};

[[nodiscard]] GpuPacketMemoryObservation ObserveGpuPacketMemory(std::uint32_t guestAddress);
[[nodiscard]] bool WaitForGpuPacketMemoryChange(GpuPacketMemoryObservation observation,
                                                std::chrono::steady_clock::time_point deadline);
void NotifyGpuPacketMemoryChanged(std::uint32_t guestAddress);

} // namespace gears
