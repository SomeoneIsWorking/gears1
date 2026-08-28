#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace gears
{

enum class FrameProductionStage
{
    SchedulerTick,
    ProducerDispatch,
    ProducerPresent,
    ProducerBlocked,
    RenderSubmission,
};

struct FrameProductionTimingReport
{
    std::uint64_t schedulerTicks = 0;
    std::uint64_t producerDispatches = 0;
    std::uint64_t producerPresents = 0;
    std::uint64_t producerBlocks = 0;
    std::uint64_t renderSubmissions = 0;
    double elapsedSeconds = 0.0;
    double schedulerHz = 0.0;
    double producerHz = 0.0;
    double presentHz = 0.0;
    double submissionHz = 0.0;
};

// Records the distinct stages between the title's timing tick and host render
// handoff. The clock is injected so the cadence calculation is independently
// testable without a game or a Vulkan device.
class FrameProductionTiming
{
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // A report is returned after at least 60 scheduler ticks and one second
    // since the previous report. Other stages only update the counters.
    std::optional<FrameProductionTimingReport> Observe(FrameProductionStage stage, TimePoint now);

  private:
    struct StageSample
    {
        std::uint64_t count = 0;
    };

    [[nodiscard]] FrameProductionTimingReport MakeReport(TimePoint now) const;

    mutable std::mutex mutex_;
    TimePoint firstEvent_{};
    TimePoint lastReport_{};
    bool started_ = false;
    std::uint64_t lastReportedSchedulerTicks_ = 0;
    StageSample samples_[5];
};

FrameProductionTiming &GlobalFrameProductionTiming();

} // namespace gears
