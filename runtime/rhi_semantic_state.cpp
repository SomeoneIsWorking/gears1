#include "rhi_semantic_state.h"

namespace gears
{

void RhiSemanticStateTracker::ApplyBinding(const RhiSemanticBinding &binding)
{
    ApplyBinding(binding, {});
}

void RhiSemanticStateTracker::ApplyBinding(const RhiSemanticBinding &binding,
                                           const RhiBindingStateEvidence &state)
{
    RhiSemanticBinding effective = binding;
    if (state.present)
    {
        effective.object = state.observedObject;
        effective.descriptor = state.descriptor;
        effective.descriptorDwords = state.descriptorDwords;
        effective.bufferViewPresent = state.bufferViewPresent;
        effective.bufferView = state.bufferView;
    }

    if (binding.kind == RhiSemanticBindingKind::ColorRenderTarget)
    {
        if (effective.object == 0)
            colorTargets_.erase(effective.slot);
        else
            colorTargets_[effective.slot] = {
                .slot = effective.slot,
                .object = effective.object,
                .descriptor = effective.descriptor,
                .descriptorDwords = effective.descriptorDwords,
            };
        return;
    }
    if (binding.kind == RhiSemanticBindingKind::DepthStencilTarget)
    {
        if (effective.object == 0)
            depthStencilTarget_.reset();
        else
            depthStencilTarget_ = {
                .depthStencil = true,
                .slot = effective.slot,
                .object = effective.object,
                .descriptor = effective.descriptor,
                .descriptorDwords = effective.descriptorDwords,
            };
        return;
    }

    if (effective.kind == RhiSemanticBindingKind::Texture)
    {
        if (effective.object == 0)
            textures_.erase(effective.slot);
        else
            textures_[effective.slot] = effective;
        return;
    }
    if (effective.kind == RhiSemanticBindingKind::PixelShader)
    {
        if (effective.object == 0)
            pixelShader_.reset();
        else
            pixelShader_ = effective;
        return;
    }
    if (effective.kind == RhiSemanticBindingKind::VertexShader)
    {
        if (effective.object == 0)
            vertexShader_.reset();
        else
            vertexShader_ = effective;
        return;
    }
    if (effective.kind == RhiSemanticBindingKind::IndexBuffer)
    {
        if (effective.object == 0 || !effective.bufferViewPresent)
            indexBuffer_.reset();
        else
            indexBuffer_ = effective;
        return;
    }
    if (effective.kind != RhiSemanticBindingKind::VertexStream)
        return;
    if (effective.object == 0 || !effective.bufferViewPresent)
    {
        vertexStreams_.erase(effective.slot);
        return;
    }

    vertexStreams_[effective.slot] = {
        .slot = effective.slot,
        .object = effective.object,
        .view = effective.bufferView,
    };
}

void RhiSemanticStateTracker::ApplyVertexStreamReset(const RhiSemanticVertexStreamReset &reset)
{
    const std::uint64_t end = std::uint64_t{reset.firstSlot} + reset.slotCount;
    for (auto stream = vertexStreams_.begin(); stream != vertexStreams_.end();)
    {
        if (stream->first >= reset.firstSlot && std::uint64_t{stream->first} < end)
            stream = vertexStreams_.erase(stream);
        else
            ++stream;
    }
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
    state.textures.reserve(textures_.size());
    for (const auto &[slot, texture] : textures_)
    {
        (void)slot;
        state.textures.push_back(texture);
    }
    state.pixelShader = pixelShader_;
    state.vertexShader = vertexShader_;
    state.indexBuffer = indexBuffer_;
    return state;
}

void RhiSemanticStateTracker::Reset()
{
    vertexStreams_.clear();
    textures_.clear();
    pixelShader_.reset();
    vertexShader_.reset();
    indexBuffer_.reset();
    colorTargets_.clear();
    depthStencilTarget_.reset();
}

} // namespace gears
