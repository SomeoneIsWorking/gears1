#include "frame_production_timing.h"

#include <cassert>
#include <chrono>
#include <cmath>

namespace
{

using Timing = gears::FrameProductionTiming;
using namespace std::chrono_literals;

void AssertNear(double actual, double expected)
{
    assert(std::abs(actual - expected) < 0.001);
}

void TestReportsAllStages()
{
    Timing timing;
    const Timing::TimePoint start{};
    std::optional<gears::FrameProductionTimingReport> report;
    for (int tick = 0; tick < 201; ++tick)
    {
        const auto now = start + tick * 10ms;
        timing.Observe(gears::FrameProductionStage::ProducerDispatch, now);
        timing.Observe(gears::FrameProductionStage::ProducerPresent, now);
        timing.Observe(gears::FrameProductionStage::ProducerBlocked, now);
        timing.Observe(gears::FrameProductionStage::RenderSubmission, now);
        const auto current = timing.Observe(gears::FrameProductionStage::SchedulerTick, now);
        if (current.has_value())
            report = current;
    }

    assert(report.has_value());
    assert(report->schedulerTicks == 201);
    assert(report->producerDispatches == 201);
    assert(report->producerPresents == 201);
    assert(report->producerBlocks == 201);
    assert(report->renderSubmissions == 201);
    AssertNear(report->elapsedSeconds, 2.0);
    AssertNear(report->schedulerHz, 100.0);
    AssertNear(report->producerHz, 100.0);
    AssertNear(report->presentHz, 100.0);
    AssertNear(report->submissionHz, 100.0);
}

void TestOnlySchedulerTicksTriggerReports()
{
    Timing timing;
    const Timing::TimePoint start{};
    for (int tick = 0; tick < 100; ++tick)
        assert(!timing.Observe(gears::FrameProductionStage::SchedulerTick, start + tick * 10ms));
    assert(!timing.Observe(gears::FrameProductionStage::ProducerDispatch, start + 590ms));
    const auto report = timing.Observe(gears::FrameProductionStage::SchedulerTick, start + 1s);
    assert(report.has_value());
    assert(report->schedulerTicks == 101);
    assert(report->producerDispatches == 1);
}

} // namespace

int main()
{
    TestReportsAllStages();
    TestOnlySchedulerTicksTriggerReports();
}
