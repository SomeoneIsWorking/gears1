#include "frame_production_timing.h"
#include "import_stub.h"

namespace
{

thread_local bool t_insideProducer = false;

void ReportSchedulerTick()
{
    const auto report = gears::GlobalFrameProductionTiming().Observe(
        gears::FrameProductionStage::SchedulerTick, gears::FrameProductionTiming::Clock::now());
    if (!report.has_value())
        return;

    gears::ReportFrameProductionTiming(*report);
}

void ObserveProducerDispatch()
{
    (void)gears::GlobalFrameProductionTiming().Observe(
        gears::FrameProductionStage::ProducerDispatch, gears::FrameProductionTiming::Clock::now());
}

void ObserveProducerPresent()
{
    (void)gears::GlobalFrameProductionTiming().Observe(gears::FrameProductionStage::ProducerPresent,
                                                       gears::FrameProductionTiming::Clock::now());
}

void ObserveProducerBlocked()
{
    (void)gears::GlobalFrameProductionTiming().Observe(gears::FrameProductionStage::ProducerBlocked,
                                                       gears::FrameProductionTiming::Clock::now());
}

} // namespace

extern "C" PPC_FUNC(__imp__sub_8221B378);
PPC_FUNC(sub_8221B378)
{
    if (gears::FrameProductionTraceEnabled())
        ReportSchedulerTick();
    __imp__sub_8221B378(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_8221B670);
PPC_FUNC(sub_8221B670)
{
    if (gears::FrameProductionTraceEnabled())
        ObserveProducerDispatch();
    const bool wasInsideProducer = t_insideProducer;
    t_insideProducer = true;
    __imp__sub_8221B670(ctx, base);
    t_insideProducer = wasInsideProducer;
}

extern "C" PPC_FUNC(__imp__sub_824A5170);
PPC_FUNC(sub_824A5170)
{
    if (gears::FrameProductionTraceEnabled())
        ObserveProducerPresent();
    __imp__sub_824A5170(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82AE8C30);
PPC_FUNC(sub_82AE8C30)
{
    const bool observe = gears::FrameProductionTraceEnabled() && t_insideProducer;
    __imp__sub_82AE8C30(ctx, base);
    if (observe && ctx.r3.u32 != 0)
        ObserveProducerBlocked();
}
