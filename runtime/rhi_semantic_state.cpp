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
        if (state.shaderModulesPresent)
            effective.shaderModules = state.shaderModules;
        else if (effective.kind == RhiSemanticBindingKind::PixelShader && pixelShader_ &&
                 pixelShader_->object == effective.object)
            effective.shaderModules = pixelShader_->shaderModules;
        else if (effective.kind == RhiSemanticBindingKind::VertexShader && vertexShader_ &&
                 vertexShader_->object == effective.object)
            effective.shaderModules = vertexShader_->shaderModules;
    }
    if (state.textureFetchStatePresent)
    {
        for (auto &[slot, texture] : textures_)
        {
            if (slot >= kRhiTextureSlotCount)
                continue;
            texture.descriptor = state.textureFetchState[slot];
            texture.descriptorDwords = kRhiTextureDescriptorDwords;
        }
    }

    if (binding.kind == RhiSemanticBindingKind::ColorRenderTarget)
    {
        if (state.surfaceStatePresent)
            surfaceState_ = state.surfaceState;
        if (effective.object == 0)
            colorTargets_.erase(effective.slot);
        else
            colorTargets_[effective.slot] = {
                .slot = effective.slot,
                .object = effective.object,
                .descriptor = effective.descriptor,
                .descriptorDwords = effective.descriptorDwords,
                .normalizedStatePresent = state.targetStatePresent,
                .normalizedState = state.targetState,
            };
        return;
    }
    if (binding.kind == RhiSemanticBindingKind::DepthStencilTarget)
    {
        if (state.surfaceStatePresent)
            surfaceState_ = state.surfaceState;
        if (effective.object == 0)
            depthStencilTarget_.reset();
        else
            depthStencilTarget_ = {
                .depthStencil = true,
                .slot = effective.slot,
                .object = effective.object,
                .descriptor = effective.descriptor,
                .descriptorDwords = effective.descriptorDwords,
                .normalizedStatePresent = state.targetStatePresent,
                .normalizedState = state.targetState,
            };
        return;
    }

    if (effective.kind == RhiSemanticBindingKind::Texture ||
        effective.kind == RhiSemanticBindingKind::TextureState)
    {
        if (effective.object == 0)
            textures_.erase(effective.slot);
        else
            textures_[effective.slot] = effective;
        return;
    }
    if (effective.kind == RhiSemanticBindingKind::PixelShader)
    {
        lastPixelShaderBinding_ = effective;
        // A command list can carry a concrete inline shader after the title's
        // API object has been cleared. Flush evidence is the binding in that
        // case; an ordinary zero-object setter remains an unbind.
        const bool concreteFlushBinding = binding.origin == RhiSemanticBindingOrigin::Flush &&
                                          state.shaderModulesPresent &&
                                          !state.shaderModules.empty();
        if (effective.object == 0 && !concreteFlushBinding)
            pixelShader_.reset();
        else
            pixelShader_ = effective;
        return;
    }
    if (effective.kind == RhiSemanticBindingKind::VertexShader)
    {
        // The same rule applies when a command-list load has no API object.
        const bool concreteFlushBinding = binding.origin == RhiSemanticBindingOrigin::Flush &&
                                          state.shaderModulesPresent &&
                                          !state.shaderModules.empty();
        if (effective.object == 0 && !concreteFlushBinding)
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

RhiColorWriteStateEvidenceResult
RhiSemanticStateTracker::ApplyColorWriteState(const RhiSemanticColorWriteState &state)
{
    constexpr std::uint32_t kColorSlot = 0;
    const auto active = colorTargets_.find(kColorSlot);
    if (!state.targetPresent)
        return active == colorTargets_.end() ? RhiColorWriteStateEvidenceResult::Match
                                             : RhiColorWriteStateEvidenceResult::Mismatch;
    if (active == colorTargets_.end())
        return RhiColorWriteStateEvidenceResult::Missing;
    if (active->second.object != state.target.object || state.target.slot != kColorSlot ||
        state.target.depthStencil)
    {
        return RhiColorWriteStateEvidenceResult::Mismatch;
    }

    active->second = state.target;
    if (state.surfaceStatePresent)
        surfaceState_ = state.surfaceState;
    return RhiColorWriteStateEvidenceResult::Match;
}

void RhiSemanticStateTracker::ApplyViewport(const RhiViewportState &state)
{
    viewportState_ = state;
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
    state.lastPixelShaderBinding = lastPixelShaderBinding_;
    state.vertexShader = vertexShader_;
    state.indexBuffer = indexBuffer_;
    if (surfaceState_.has_value())
    {
        state.surfaceStatePresent = true;
        state.surfaceState = *surfaceState_;
    }
    if (viewportState_.has_value())
    {
        state.viewportStatePresent = true;
        state.viewportState = *viewportState_;
    }
    return state;
}

void RhiSemanticStateTracker::Reset()
{
    vertexStreams_.clear();
    textures_.clear();
    pixelShader_.reset();
    lastPixelShaderBinding_.reset();
    vertexShader_.reset();
    indexBuffer_.reset();
    colorTargets_.clear();
    depthStencilTarget_.reset();
    surfaceState_.reset();
    viewportState_.reset();
}

} // namespace gears
