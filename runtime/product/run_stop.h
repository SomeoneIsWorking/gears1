#pragma once

#include <atomic>

namespace gears::product
{

// A maintainer's request, from the control channel, to end an offscreen run
// before its --seconds. The run ends at its next whole second and still runs
// its end-of-run checks, so an early end is evidence like a full one.
class RunStop final
{
  public:
    void Request() noexcept { requested_.store(true, std::memory_order_relaxed); }

    [[nodiscard]] bool Requested() const noexcept
    {
        return requested_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<bool> requested_{false};
};

} // namespace gears::product
