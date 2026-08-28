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
    RenderRingReservation,
    PresentBoundary,
};

struct FrameProductionTimingReport
{
    std::uint64_t schedulerTicks = 0;
    std::uint64_t producerDispatches = 0;
    std::uint64_t producerPresents = 0;
    std::uint64_t producerBlocks = 0;
    std::uint64_t renderSubmissions = 0;
    std::uint64_t renderRingReservations = 0;
    std::uint64_t presentBoundaries = 0;
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

    // A scheduler-driven report is returned after at least 60 scheduler ticks
    // and one second since the previous report. Other stages only update the
    // counters; ReportIfDue provides the post-scheduler boundary.
    std::optional<FrameProductionTimingReport> Observe(FrameProductionStage stage, TimePoint now);

    // The scheduler tick is a startup-only boundary for this title. Once it
    // stops, the semantic present boundary can still drive reports so the
    // post-Bink producer remains measurable without inventing a second timing
    // owner.
    std::optional<FrameProductionTimingReport> ReportIfDue(TimePoint now);

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
    std::uint64_t eventCount_ = 0;
    std::uint64_t lastReportedEvents_ = 0;
    StageSample samples_[7];
};

FrameProductionTiming &GlobalFrameProductionTiming();

void ReportFrameProductionTiming(const FrameProductionTimingReport &report);
[[nodiscard]] bool FrameProductionTraceEnabled();
void ObserveFrameProductionRingReservation();
void ObserveFrameProductionPresentBoundary();

} // namespace gears
