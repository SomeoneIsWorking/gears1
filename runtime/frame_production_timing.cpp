#include "frame_production_timing.h"

#include <cstddef>

namespace gears
{

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
    if (stage != FrameProductionStage::SchedulerTick ||
        sample.count < lastReportedSchedulerTicks_ + kMinimumReportInterval ||
        now - lastReport_ < kMinimumReportDuration)
        return std::nullopt;

    lastReport_ = now;
    lastReportedSchedulerTicks_ = sample.count;
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

} // namespace gears
