#include "frame_production_timing.h"

#include <cstddef>

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears
{

bool FrameProductionTraceEnabled()
{
    static const bool enabled = lucent::config::flag("FRAME_PRODUCTION_TRACE");
    return enabled;
}

namespace
{

constexpr std::uint64_t kMinimumReportInterval = 60;
constexpr auto kMinimumReportDuration = std::chrono::seconds(1);

[[nodiscard]] std::size_t StageIndex(FrameProductionStage stage)
{
    switch (stage)
    {
    case FrameProductionStage::SchedulerTick:
        return 0;
    case FrameProductionStage::ProducerDispatch:
        return 1;
    case FrameProductionStage::ProducerPresent:
        return 2;
    case FrameProductionStage::ProducerBlocked:
        return 3;
    case FrameProductionStage::RenderSubmission:
        return 4;
    case FrameProductionStage::RenderRingReservation:
        return 5;
    case FrameProductionStage::PresentBoundary:
        return 6;
    }
    return 0;
}

[[nodiscard]] double Rate(std::uint64_t count, double elapsedSeconds)
{
    if (count < 2 || elapsedSeconds <= 0.0)
        return 0.0;
    return double(count - 1) / elapsedSeconds;
}

} // namespace

std::optional<FrameProductionTimingReport>
FrameProductionTiming::Observe(FrameProductionStage stage, TimePoint now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_)
    {
        firstEvent_ = now;
        lastReport_ = now;
        started_ = true;
    }

    StageSample &sample = samples_[StageIndex(stage)];
    ++sample.count;
    ++eventCount_;
    if (stage != FrameProductionStage::SchedulerTick ||
        sample.count < lastReportedSchedulerTicks_ + kMinimumReportInterval ||
        now - lastReport_ < kMinimumReportDuration)
        return std::nullopt;

    lastReport_ = now;
    lastReportedSchedulerTicks_ = sample.count;
    lastReportedEvents_ = eventCount_;
    return MakeReport(now);
}

std::optional<FrameProductionTimingReport> FrameProductionTiming::ReportIfDue(TimePoint now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || eventCount_ < lastReportedEvents_ + kMinimumReportInterval ||
        now - lastReport_ < kMinimumReportDuration)
        return std::nullopt;

    lastReport_ = now;
    lastReportedEvents_ = eventCount_;
    return MakeReport(now);
}

FrameProductionTimingReport FrameProductionTiming::MakeReport(TimePoint now) const
{
    const double elapsedSeconds = std::chrono::duration<double>(now - firstEvent_).count();
    return {
        .schedulerTicks = samples_[0].count,
        .producerDispatches = samples_[1].count,
        .producerPresents = samples_[2].count,
        .producerBlocks = samples_[3].count,
        .renderSubmissions = samples_[4].count,
        .renderRingReservations = samples_[5].count,
        .presentBoundaries = samples_[6].count,
        .elapsedSeconds = elapsedSeconds,
        .schedulerHz = Rate(samples_[0].count, elapsedSeconds),
        .producerHz = Rate(samples_[1].count, elapsedSeconds),
        .presentHz = Rate(samples_[2].count, elapsedSeconds),
        .submissionHz = Rate(samples_[4].count, elapsedSeconds),
    };
}

FrameProductionTiming &GlobalFrameProductionTiming()
{
    static FrameProductionTiming timing;
    return timing;
}

void ReportFrameProductionTiming(const FrameProductionTimingReport &report)
{
    lucent::info("timing",
                 "Gears 1 frame chain: scheduler {:.1f}/s ({}), producer {:.1f}/s ({}),"
                 " producer-present {} calls, blocked {} calls, render-ring reserve {:.1f}/s ({}),"
                 " present boundary {:.1f}/s ({}), render handoff {:.1f}/s ({}), over {:.2f}s",
                 report.schedulerHz, report.schedulerTicks, report.producerHz,
                 report.producerDispatches, report.producerPresents, report.producerBlocks,
                 Rate(report.renderRingReservations, report.elapsedSeconds),
                 report.renderRingReservations,
                 Rate(report.presentBoundaries, report.elapsedSeconds), report.presentBoundaries,
                 report.submissionHz, report.renderSubmissions, report.elapsedSeconds);
}

void ObserveFrameProductionRingReservation()
{
    if (!FrameProductionTraceEnabled())
        return;
    (void)GlobalFrameProductionTiming().Observe(FrameProductionStage::RenderRingReservation,
                                                FrameProductionTiming::Clock::now());
}

void ObserveFrameProductionPresentBoundary()
{
    if (!FrameProductionTraceEnabled())
        return;
    auto &timing = GlobalFrameProductionTiming();
    const auto now = FrameProductionTiming::Clock::now();
    (void)timing.Observe(FrameProductionStage::PresentBoundary, now);
    if (const auto report = timing.ReportIfDue(now))
        ReportFrameProductionTiming(*report);
}

} // namespace gears
