#include "gpu_draw.h"
#include "rhi_semantic_stream.h"

namespace gears
{

void ObserveRhiRendererFrameInputs(std::uint64_t frameSequence,
                                   const std::vector<FrameDrawItem> &draws)
{
    if (!RhiSemanticObservationEnabled())
        return;

    std::vector<RhiRendererDrawInput> rendererDraws;
    rendererDraws.reserve(draws.size());
    for (const FrameDrawItem &draw : draws)
        rendererDraws.push_back({.primitiveType = draw.primType,
                                 .elementCount = draw.indexCount,
                                 .indexed = draw.indexed,
                                 .indexIs32 = draw.indexIs32,
                                 .indexEndian = draw.indexEndian});
    ObserveRhiRendererDraws(frameSequence, rendererDraws);
}

} // namespace gears
