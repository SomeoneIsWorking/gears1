#include "rhi_semantic_state.h"

namespace gears
{

void RhiSemanticStateTracker::ApplyBinding(const RhiSemanticBinding &binding)
{
    if (binding.kind != RhiSemanticBindingKind::VertexStream)
        return;
    if (binding.object == 0 || !binding.bufferViewPresent)
    {
        vertexStreams_.erase(binding.slot);
        return;
    }

    vertexStreams_[binding.slot] = {
        .slot = binding.slot,
        .object = binding.object,
        .view = binding.bufferView,
    };
}

RhiSemanticDrawState RhiSemanticStateTracker::SnapshotDraw(const RhiSemanticDraw &draw) const
{
    RhiSemanticDrawState state{.draw = draw};
    state.vertexStreams.reserve(vertexStreams_.size());
    for (const auto &[slot, stream] : vertexStreams_)
    {
        (void)slot;
        state.vertexStreams.push_back(stream);
    }
    return state;
}

void RhiSemanticStateTracker::Reset()
{
    vertexStreams_.clear();
}

} // namespace gears
