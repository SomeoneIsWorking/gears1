#include "native_rhi.h"
#include "frame_production_timing.h"

namespace gears::native_rhi
{

void ObserveAndSubmitPresent(const RhiSemanticPresent &present,
                             const RhiPresentPacketEvidence &packet)
{
    ObserveRhiSemanticPresent(present, packet);
    ObserveFrameProductionPresentBoundary();
    const RhiSemanticFrame frame = ReportRhiSemanticFrame(present.frameSequence);
    SubmitSemanticFrame(frame);
}

} // namespace gears::native_rhi
