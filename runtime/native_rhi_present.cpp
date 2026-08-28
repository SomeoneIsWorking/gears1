#include "native_rhi.h"

namespace gears::native_rhi
{

void ObserveAndSubmitPresent(const RhiSemanticPresent &present,
                             const RhiPresentPacketEvidence &packet)
{
    ObserveRhiSemanticPresent(present, packet);
    const RhiSemanticFrame frame = ReportRhiSemanticFrame(present.frameSequence);
    SubmitSemanticFrame(frame);
}

} // namespace gears::native_rhi
