#include "rhi_semantic_state.h"

namespace gears
{

void RhiSemanticStateTracker::ApplyBinding(const RhiSemanticBinding &binding)
{
    if (binding.kind == RhiSemanticBindingKind::ColorRenderTarget)
    {
        if (binding.object == 0)
        {
            colorTargets_.erase(binding.slot);
            return;
        }
        colorTargets_[binding.slot] = {
            .slot = binding.slot,
            .object = binding.object,
        };
        return;
    }
    if (binding.kind == RhiSemanticBindingKind::DepthStencilTarget)
    {
        if (binding.object == 0)
        {
            depthStencilTarget_.reset();
            return;
        }
        depthStencilTarget_ = {
            .depthStencil = true,
            .slot = binding.slot,
            .object = binding.object,
        };
        return;
    }
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
    state.renderTargets.reserve(colorTargets_.size() + (depthStencilTarget_.has_value() ? 1 : 0));
    for (const auto &[slot, target] : colorTargets_)
    {
        (void)slot;
        state.renderTargets.push_back(target);
    }
    if (depthStencilTarget_.has_value())
        state.renderTargets.push_back(*depthStencilTarget_);
    return state;
}

void RhiSemanticStateTracker::Reset()
{
    vertexStreams_.clear();
    colorTargets_.clear();
    depthStencilTarget_.reset();
}

} // namespace gears
