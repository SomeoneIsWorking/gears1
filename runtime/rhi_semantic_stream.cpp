#include "rhi_semantic_stream.h"

#include "rhi_pm4_shader_evidence.h"
#include "rhi_renderer_input.h"
#include "rhi_semantic_state.h"
#include "guest_address.h"

#include <array>
#include <map>
#include <mutex>
#include <utility>

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears
{
namespace
{

struct RhiSemanticStreamState
{
    std::mutex mutex;
    std::vector<RhiSemanticEvent> pendingEvents;
    std::uint64_t nextSequence = 1;
    RhiSemanticStateTracker semanticState;
};

RhiSemanticStreamState g_stream;

std::mutex g_shaderPacketEvidenceMutex;
std::map<std::uint32_t, RhiShaderPacketModuleEvidence> g_shaderPacketEvidence;

[[nodiscard]] std::uint32_t CanonicalShaderPacketAddress(std::uint32_t address)
{
    return address & kGuestPhysicalAddressMask & ~std::uint32_t{3};
}

struct RhiSemanticReportTotals
{
    std::uint64_t draws = 0;
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t mismatched = 0;
    std::array<std::uint64_t, 4> drawKinds{};
    std::uint64_t boundDrawsWithVertexStreams = 0;
    std::uint64_t boundDrawsWithoutVertexStreams = 0;
    std::uint64_t drawsWithRenderTargets = 0;
    std::uint64_t drawsWithoutRenderTargets = 0;
    std::uint64_t bindings = 0;
    std::uint64_t bindingsMatched = 0;
    std::uint64_t bindingsMissing = 0;
    std::uint64_t bindingsMismatched = 0;
    std::array<std::uint64_t, kRhiSemanticBindingKindCount> bindingKinds{};
    std::uint64_t resourceLifetimeCalls = 0;
    std::uint64_t resourceLifetimeMatched = 0;
    std::uint64_t resourceLifetimeMissing = 0;
    std::uint64_t resourceLifetimeMismatched = 0;
    std::uint64_t resourceRetirements = 0;
    std::uint64_t resourceConstructions = 0;
    std::uint64_t resourceAddReferences = 0;
    std::uint64_t resourceReleases = 0;
    std::array<std::uint64_t, 16> resourceTypes{};
    std::uint64_t vertexStreamResets = 0;
    std::uint64_t vertexStreamResetsMatched = 0;
    std::uint64_t vertexStreamResetsMissing = 0;
    std::uint64_t vertexStreamResetsMismatched = 0;
    std::uint64_t colorWriteStates = 0;
    std::uint64_t colorWriteStatesMatched = 0;
    std::uint64_t colorWriteStatesMissing = 0;
    std::uint64_t colorWriteStatesMismatched = 0;
    std::uint64_t resolves = 0;
    std::uint64_t resolvesMatched = 0;
    std::uint64_t resolvesMissing = 0;
    std::uint64_t resolvesMismatched = 0;
    std::uint64_t presents = 0;
    std::uint64_t presentsMatched = 0;
    std::uint64_t presentsMissing = 0;
    std::uint64_t presentsMismatched = 0;
};

RhiSemanticReportTotals g_reportTotals;

[[nodiscard]] bool ExpectsTransientData(RhiSemanticDrawKind kind)
{
    return kind == RhiSemanticDrawKind::TransientVertices ||
           kind == RhiSemanticDrawKind::TransientVerticesAndIndices;
}

[[nodiscard]] bool ExpectsBoundVertexState(RhiSemanticDrawKind kind)
{
    return kind == RhiSemanticDrawKind::BoundVertices || kind == RhiSemanticDrawKind::BoundIndices;
}

[[nodiscard]] bool SameBufferRange(const RhiSemanticBufferRange &left,
                                   const RhiSemanticBufferRange &right)
{
    return left.guestAddress == right.guestAddress && left.sizeBytes == right.sizeBytes;
}

[[nodiscard]] bool SameBufferView(const RhiSemanticBufferView &left,
                                  const RhiSemanticBufferView &right)
{
    return SameBufferRange(left.allocation, right.allocation) &&
           left.elementStrideBytes == right.elementStrideBytes &&
           left.endianSwap == right.endianSwap;
}

[[nodiscard]] bool SameVertexStream(const RhiSemanticVertexStream &left,
                                    const RhiSemanticVertexStream &right)
{
    return left.slot == right.slot && left.object == right.object &&
           SameBufferView(left.view, right.view);
}

[[nodiscard]] const RhiSemanticVertexStream &
VertexStreamOrEmpty(const std::vector<RhiSemanticVertexStream> &streams, std::size_t index)
{
    static const RhiSemanticVertexStream empty;
    return index < streams.size() ? streams[index] : empty;
}

[[nodiscard]] bool SameRenderTarget(const RhiSemanticRenderTarget &left,
                                    const RhiSemanticRenderTarget &right)
{
    return left.depthStencil == right.depthStencil && left.slot == right.slot &&
           left.object == right.object;
}

[[nodiscard]] const RhiSemanticRenderTarget &
RenderTargetOrEmpty(const std::vector<RhiSemanticRenderTarget> &targets, std::size_t index)
{
    static const RhiSemanticRenderTarget empty;
    return index < targets.size() ? targets[index] : empty;
}

[[nodiscard]] const char *DrawKindName(RhiSemanticDrawKind kind)
{
    switch (kind)
    {
    case RhiSemanticDrawKind::TransientVertices:
        return "transient-vertices";
    case RhiSemanticDrawKind::TransientVerticesAndIndices:
        return "transient-vertices+indices";
    case RhiSemanticDrawKind::BoundVertices:
        return "bound-vertices";
    case RhiSemanticDrawKind::BoundIndices:
        return "bound-indices";
    }
    return "unknown";
}

[[nodiscard]] const char *BindingKindName(RhiSemanticBindingKind kind)
{
    switch (kind)
    {
    case RhiSemanticBindingKind::Texture:
        return "texture";
    case RhiSemanticBindingKind::TextureState:
        return "texture-state";
    case RhiSemanticBindingKind::PixelShader:
        return "pixel-shader";
    case RhiSemanticBindingKind::VertexShader:
        return "vertex-shader";
    case RhiSemanticBindingKind::VertexStream:
        return "vertex-stream";
    case RhiSemanticBindingKind::IndexBuffer:
        return "index-buffer";
    case RhiSemanticBindingKind::ColorRenderTarget:
        return "color-render-target";
    case RhiSemanticBindingKind::DepthStencilTarget:
        return "depth-stencil-target";
    }
    return "unknown";
}

} // namespace

bool RhiSemanticObservationEnabled()
{
    static const bool enabled =
        lucent::config::flag("NATIVE_RHI_OBSERVE") || lucent::config::flag("NATIVE_RHI_PLAN");
    return enabled;
}

RhiDrawEvidenceResult CompareRhiDrawPacket(const RhiSemanticDraw &draw,
                                           const RhiDrawPacketEvidence &packet)
{
    if (!packet.present)
        return RhiDrawEvidenceResult::Missing;

    constexpr std::uint32_t kDrawIndx = 0x22;
    constexpr std::uint32_t kDrawIndx2 = 0x36;
    const bool drawOpcode = packet.opcode == kDrawIndx || packet.opcode == kDrawIndx2;
    const std::uint32_t expectedSource = RhiDrawUsesDmaIndices(draw.kind) ? 0u : 2u;
    if (!drawOpcode || packet.primitiveType != (draw.primitiveType & 0x3Fu) ||
        packet.sourceSelect != expectedSource || packet.elementCount != draw.elementCount)
    {
        return RhiDrawEvidenceResult::Mismatch;
    }
    if (ExpectsTransientData(draw.kind))
    {
        if (!packet.transientDataPresent)
            return RhiDrawEvidenceResult::Missing;
        if (packet.vertexData.guestAddress != draw.vertexData.guestAddress ||
            packet.vertexData.sizeBytes != draw.vertexData.sizeBytes ||
            packet.indexData.guestAddress != draw.indexData.guestAddress ||
            packet.indexData.sizeBytes != draw.indexData.sizeBytes)
        {
            return RhiDrawEvidenceResult::Mismatch;
        }
    }
    if (draw.kind == RhiSemanticDrawKind::BoundIndices)
    {
        if (!draw.indexBufferViewPresent)
            return RhiDrawEvidenceResult::Mismatch;
        if (!packet.indexDataPresent)
            return RhiDrawEvidenceResult::Missing;
        if (!SameBufferRange(packet.indexData, draw.indexData) ||
            packet.indexStrideBytes != draw.indexBuffer.elementStrideBytes ||
            packet.indexEndianSwap != draw.indexBuffer.endianSwap)
        {
            return RhiDrawEvidenceResult::Mismatch;
        }
    }
    return RhiDrawEvidenceResult::Match;
}

RhiDrawEvidenceResult CompareRhiDrawVertexState(const RhiSemanticDrawState &state,
                                                const RhiDrawPacketEvidence &packet)
{
    if (!ExpectsBoundVertexState(state.draw.kind))
        return RhiDrawEvidenceResult::Match;
    if (!packet.vertexStreamsPresent)
        return RhiDrawEvidenceResult::Missing;
    if (state.vertexStreams.size() != packet.vertexStreams.size())
        return RhiDrawEvidenceResult::Mismatch;
    for (std::size_t index = 0; index < state.vertexStreams.size(); ++index)
    {
        if (!SameVertexStream(state.vertexStreams[index], packet.vertexStreams[index]))
            return RhiDrawEvidenceResult::Mismatch;
    }
    return RhiDrawEvidenceResult::Match;
}

RhiDrawEvidenceResult CompareRhiDrawRenderTargetState(const RhiSemanticDrawState &state,
                                                      const RhiDrawPacketEvidence &packet)
{
    if (!packet.renderTargetsPresent)
        return RhiDrawEvidenceResult::Missing;
    if (state.renderTargets.size() != packet.renderTargets.size())
        return RhiDrawEvidenceResult::Mismatch;
    for (std::size_t index = 0; index < state.renderTargets.size(); ++index)
    {
        if (!SameRenderTarget(state.renderTargets[index], packet.renderTargets[index]))
            return RhiDrawEvidenceResult::Mismatch;
    }
    return RhiDrawEvidenceResult::Match;
}

RhiBindingEvidenceResult CompareRhiBindingState(const RhiSemanticBinding &binding,
                                                const RhiBindingStateEvidence &state)
{
    if (!state.present)
        return RhiBindingEvidenceResult::Missing;
    if (state.observedObject != binding.object)
        return RhiBindingEvidenceResult::Mismatch;
    if (binding.descriptorDwords != 0)
    {
        if (state.descriptorDwords != binding.descriptorDwords)
            return RhiBindingEvidenceResult::Mismatch;
        for (std::uint32_t index = 0; index < binding.descriptorDwords; ++index)
        {
            if (state.descriptor[index] != binding.descriptor[index])
                return RhiBindingEvidenceResult::Mismatch;
        }
    }
    if (binding.bufferViewPresent != state.bufferViewPresent)
        return RhiBindingEvidenceResult::Mismatch;
    if (binding.bufferViewPresent && !SameBufferView(binding.bufferView, state.bufferView))
        return RhiBindingEvidenceResult::Mismatch;
    return RhiBindingEvidenceResult::Match;
}

RhiPresentEvidenceResult CompareRhiPresentPacket(const RhiSemanticPresent &present,
                                                 const RhiPresentPacketEvidence &packet)
{
    if (!packet.present)
        return RhiPresentEvidenceResult::Missing;
    if (!packet.framingValid ||
        packet.frameSequence != static_cast<std::uint32_t>(present.frameSequence) ||
        packet.frontBuffer != present.frontBuffer ||
        packet.fetchDescriptor != present.fetchDescriptor)
    {
        return RhiPresentEvidenceResult::Mismatch;
    }
    return RhiPresentEvidenceResult::Match;
}

RhiResourceLifetimeEvidenceResult
CompareRhiResourceLifetime(const RhiSemanticResourceLifetime &lifetime,
                           const RhiResourceLifetimeEvidence &retained)
{
    if (!retained.present)
        return RhiResourceLifetimeEvidenceResult::Missing;
    const std::uint32_t expectedReferenceCount =
        lifetime.operation == RhiResourceLifetimeOperation::AddReference
            ? lifetime.previousReferenceCount + 1
            : lifetime.previousReferenceCount - 1;
    return retained.returnedReferenceCount == expectedReferenceCount
               ? RhiResourceLifetimeEvidenceResult::Match
               : RhiResourceLifetimeEvidenceResult::Mismatch;
}

RhiVertexStreamResetEvidenceResult
CompareRhiVertexStreamReset(const RhiSemanticVertexStreamReset &reset,
                            const RhiVertexStreamResetEvidence &state)
{
    if (!state.present)
        return RhiVertexStreamResetEvidenceResult::Missing;
    const std::uint64_t end = std::uint64_t{reset.firstSlot} + reset.slotCount;
    for (const RhiSemanticVertexStream &stream : state.activeStreams)
    {
        if (stream.slot >= reset.firstSlot && std::uint64_t{stream.slot} < end)
            return RhiVertexStreamResetEvidenceResult::Mismatch;
    }
    return RhiVertexStreamResetEvidenceResult::Match;
}

RhiResolveEvidenceResult CompareRhiResolvePacket(const RhiSemanticResolve &resolve,
                                                 const RhiResolvePacketEvidence &packet)
{
    if (!packet.present)
        return RhiResolveEvidenceResult::Missing;

    constexpr std::uint32_t kDrawIndx = 0x22;
    constexpr std::uint32_t kDrawIndx2 = 0x36;
    constexpr std::uint32_t kRectangleList = 8;
    constexpr std::uint32_t kAutoIndex = 2;
    constexpr std::uint32_t kRectangleVertices = 3;
    const bool drawOpcode = packet.drawOpcode == kDrawIndx || packet.drawOpcode == kDrawIndx2;
    if (packet.observedSourceObject != resolve.sourceObject ||
        packet.destinationAddress != resolve.destinationAddress ||
        packet.destinationPitch != resolve.destinationPitch ||
        packet.destinationHeight != resolve.destinationHeight || !drawOpcode ||
        packet.primitiveType != kRectangleList || packet.sourceSelect != kAutoIndex ||
        packet.elementCount != kRectangleVertices)
    {
        return RhiResolveEvidenceResult::Mismatch;
    }
    return RhiResolveEvidenceResult::Match;
}

void ObserveRhiSemanticDraw(const RhiSemanticDraw &draw, const RhiDrawPacketEvidence &packet)
{
    if (!RhiSemanticObservationEnabled())
        return;

    std::lock_guard guard(g_stream.mutex);
    RhiSemanticDrawState state = g_stream.semanticState.SnapshotDraw(draw);
    RhiDrawEvidenceResult evidence = CompareRhiDrawPacket(draw, packet);
    if (evidence == RhiDrawEvidenceResult::Match)
        evidence = CompareRhiDrawVertexState(state, packet);
    if (evidence == RhiDrawEvidenceResult::Match)
        evidence = CompareRhiDrawRenderTargetState(state, packet);
    g_stream.pendingEvents.push_back(
        {.sequence = g_stream.nextSequence++,
         .payload =
             RhiObservedDraw{.state = std::move(state), .packet = packet, .evidence = evidence}});
}

void ObserveRhiSemanticBinding(const RhiSemanticBinding &binding,
                               const RhiBindingStateEvidence &state)
{
    if (!RhiSemanticObservationEnabled())
        return;

    std::lock_guard guard(g_stream.mutex);
    g_stream.pendingEvents.push_back(
        {.sequence = g_stream.nextSequence++,
         .payload = RhiObservedBinding{.binding = binding,
                                       .state = state,
                                       .evidence = CompareRhiBindingState(binding, state)}});
    g_stream.semanticState.ApplyBinding(binding, state);
}

void ObserveRhiShaderPacketModuleEvidence(
    const std::vector<RhiShaderPacketModuleEvidence> &evidence)
{
    if (!RhiSemanticObservationEnabled())
        return;
    std::lock_guard guard(g_shaderPacketEvidenceMutex);
    for (const RhiShaderPacketModuleEvidence &entry : evidence)
    {
        const std::uint32_t packet = CanonicalShaderPacketAddress(entry.packetGuestAddress);
        if (packet != 0)
            g_shaderPacketEvidence.insert_or_assign(packet, entry);
    }
}

void ApplyRhiShaderPacketModuleEvidence(RhiSemanticDrawState &state,
                                        std::uint32_t packetGuestAddress)
{
    const std::uint32_t packet = CanonicalShaderPacketAddress(packetGuestAddress);
    if (packet == 0)
        return;
    std::lock_guard guard(g_shaderPacketEvidenceMutex);
    const auto it = g_shaderPacketEvidence.find(packet);
    if (it == g_shaderPacketEvidence.end())
        return;
    const RhiShaderPacketModuleEvidence &evidence = it->second;
    if (!evidence.vertexModules.empty())
    {
        if (!state.vertexShader.has_value())
            state.vertexShader = {.kind = RhiSemanticBindingKind::VertexShader,
                                  .origin = RhiSemanticBindingOrigin::Flush,
                                  .shaderModules = evidence.vertexModules};
        else if (state.vertexShader->shaderModules.empty())
            state.vertexShader->shaderModules = evidence.vertexModules;
    }
    if (!evidence.pixelModules.empty())
    {
        if (!state.pixelShader.has_value())
            state.pixelShader = {.kind = RhiSemanticBindingKind::PixelShader,
                                 .origin = RhiSemanticBindingOrigin::Flush,
                                 .shaderModules = evidence.pixelModules};
        else if (state.pixelShader->shaderModules.empty())
            state.pixelShader->shaderModules = evidence.pixelModules;
    }
}

void ObserveRhiSemanticPresent(const RhiSemanticPresent &present,
                               const RhiPresentPacketEvidence &packet)
{
    if (!RhiSemanticObservationEnabled())
        return;

    std::lock_guard guard(g_stream.mutex);
    g_stream.pendingEvents.push_back(
        {.sequence = g_stream.nextSequence++,
         .payload = RhiObservedPresent{.present = present,
                                       .packet = packet,
                                       .evidence = CompareRhiPresentPacket(present, packet)}});
}

void ObserveRhiSemanticResourceLifetime(const RhiSemanticResourceLifetime &lifetime,
                                        const RhiResourceLifetimeEvidence &retained)
{
    if (!RhiSemanticObservationEnabled())
        return;

    std::lock_guard guard(g_stream.mutex);
    g_stream.pendingEvents.push_back(
        {.sequence = g_stream.nextSequence++,
         .payload = RhiObservedResourceLifetime{
             .lifetime = lifetime,
             .retained = retained,
             .evidence = CompareRhiResourceLifetime(lifetime, retained)}});
}

void ObserveRhiSemanticResourceConstruction(const RhiSemanticResourceConstruction &construction,
                                            const RhiResourceConstructionEvidence &retained)
{
    if (!RhiSemanticObservationEnabled())
        return;

    std::lock_guard guard(g_stream.mutex);
    g_stream.pendingEvents.push_back({.sequence = g_stream.nextSequence++,
                                      .payload = RhiObservedResourceConstruction{
                                          .construction = construction, .retained = retained}});
}

void ObserveRhiSemanticVertexStreamReset(const RhiSemanticVertexStreamReset &reset,
                                         const RhiVertexStreamResetEvidence &state)
{
    if (!RhiSemanticObservationEnabled())
        return;

    std::lock_guard guard(g_stream.mutex);
    g_stream.pendingEvents.push_back({.sequence = g_stream.nextSequence++,
                                      .payload = RhiObservedVertexStreamReset{
                                          .reset = reset,
                                          .state = state,
                                          .evidence = CompareRhiVertexStreamReset(reset, state)}});
    g_stream.semanticState.ApplyVertexStreamReset(reset);
}

void ObserveRhiSemanticColorWriteState(const RhiSemanticColorWriteState &state)
{
    if (!RhiSemanticObservationEnabled())
        return;

    std::lock_guard guard(g_stream.mutex);
    const RhiColorWriteStateEvidenceResult evidence =
        g_stream.semanticState.ApplyColorWriteState(state);
    g_stream.pendingEvents.push_back(
        {.sequence = g_stream.nextSequence++,
         .payload = RhiObservedColorWriteState{.state = state, .evidence = evidence}});
}
void ObserveRhiSemanticViewport(const RhiViewportState &state)
{
    if (!RhiSemanticObservationEnabled())
        return;
    std::lock_guard guard(g_stream.mutex);
    g_stream.pendingEvents.push_back(
        {.sequence = g_stream.nextSequence++, .payload = RhiObservedViewport{.state = state}});
    g_stream.semanticState.ApplyViewport(state);
}
void ObserveRhiSemanticResolve(const RhiSemanticResolve &resolve,
                               const RhiResolvePacketEvidence &packet)
{
    if (!RhiSemanticObservationEnabled())
        return;

    std::lock_guard guard(g_stream.mutex);
    g_stream.pendingEvents.push_back(
        {.sequence = g_stream.nextSequence++,
         .payload = RhiObservedResolve{.resolve = resolve,
                                       .packet = packet,
                                       .evidence = CompareRhiResolvePacket(resolve, packet)}});
}

RhiSemanticFrame SealRhiSemanticFrame(std::uint64_t frameSequence)
{
    RhiSemanticFrame frame{.frameSequence = frameSequence};
    {
        std::lock_guard guard(g_stream.mutex);
        frame.events = std::move(g_stream.pendingEvents);
        g_stream.pendingEvents.clear();
    }
    for (const RhiSemanticEvent &event : frame.events)
    {
        if (const auto *observed = std::get_if<RhiObservedDraw>(&event.payload))
        {
            ++frame.draws;
            switch (observed->evidence)
            {
            case RhiDrawEvidenceResult::Match:
                ++frame.matched;
                break;
            case RhiDrawEvidenceResult::Missing:
                ++frame.missing;
                break;
            case RhiDrawEvidenceResult::Mismatch:
                ++frame.mismatched;
                break;
            }
            continue;
        }
        if (const auto *observed = std::get_if<RhiObservedBinding>(&event.payload))
        {
            ++frame.bindings;
            switch (observed->evidence)
            {
            case RhiBindingEvidenceResult::Match:
                ++frame.bindingsMatched;
                break;
            case RhiBindingEvidenceResult::Missing:
                ++frame.bindingsMissing;
                break;
            case RhiBindingEvidenceResult::Mismatch:
                ++frame.bindingsMismatched;
                break;
            }
            continue;
        }
        if (const auto *observed = std::get_if<RhiObservedResourceLifetime>(&event.payload))
        {
            ++frame.resourceLifetimeCalls;
            if (observed->lifetime.operation == RhiResourceLifetimeOperation::Release &&
                observed->retained.present && observed->retained.returnedReferenceCount == 0)
            {
                ++frame.resourceRetirements;
            }
            switch (observed->evidence)
            {
            case RhiResourceLifetimeEvidenceResult::Match:
                ++frame.resourceLifetimeMatched;
                break;
            case RhiResourceLifetimeEvidenceResult::Missing:
                ++frame.resourceLifetimeMissing;
                break;
            case RhiResourceLifetimeEvidenceResult::Mismatch:
                ++frame.resourceLifetimeMismatched;
                break;
            }
            continue;
        }
        if (std::holds_alternative<RhiObservedResourceConstruction>(event.payload))
        {
            ++frame.resourceConstructions;
            continue;
        }
        if (const auto *observed = std::get_if<RhiObservedVertexStreamReset>(&event.payload))
        {
            ++frame.vertexStreamResets;
            switch (observed->evidence)
            {
            case RhiVertexStreamResetEvidenceResult::Match:
                ++frame.vertexStreamResetsMatched;
                break;
            case RhiVertexStreamResetEvidenceResult::Missing:
                ++frame.vertexStreamResetsMissing;
                break;
            case RhiVertexStreamResetEvidenceResult::Mismatch:
                ++frame.vertexStreamResetsMismatched;
                break;
            }
            continue;
        }
        if (const auto *observed = std::get_if<RhiObservedColorWriteState>(&event.payload))
        {
            ++frame.colorWriteStates;
            switch (observed->evidence)
            {
            case RhiColorWriteStateEvidenceResult::Match:
                ++frame.colorWriteStatesMatched;
                break;
            case RhiColorWriteStateEvidenceResult::Missing:
                ++frame.colorWriteStatesMissing;
                break;
            case RhiColorWriteStateEvidenceResult::Mismatch:
                ++frame.colorWriteStatesMismatched;
                break;
            }
            continue;
        }
        if (std::holds_alternative<RhiObservedViewport>(event.payload))
            continue;
        if (const auto *observed = std::get_if<RhiObservedResolve>(&event.payload))
        {
            ++frame.resolves;
            switch (observed->evidence)
            {
            case RhiResolveEvidenceResult::Match:
                ++frame.resolvesMatched;
                break;
            case RhiResolveEvidenceResult::Missing:
                ++frame.resolvesMissing;
                break;
            case RhiResolveEvidenceResult::Mismatch:
                ++frame.resolvesMismatched;
                break;
            }
            continue;
        }

        const auto &observed = std::get<RhiObservedPresent>(event.payload);
        ++frame.presents;
        switch (observed.evidence)
        {
        case RhiPresentEvidenceResult::Match:
            ++frame.presentsMatched;
            break;
        case RhiPresentEvidenceResult::Missing:
            ++frame.presentsMissing;
            break;
        case RhiPresentEvidenceResult::Mismatch:
            ++frame.presentsMismatched;
            break;
        }
    }
    (void)ObserveRhiSemanticFrameSealed(frame);
    (void)ObserveRhiSemanticFrameForPm4ShaderEvidence(frame);
    return frame;
}

RhiSemanticFrame ReportRhiSemanticFrame(std::uint64_t frameSequence)
{
    if (!RhiSemanticObservationEnabled())
        return {.frameSequence = frameSequence};

    const RhiSemanticFrame frame = SealRhiSemanticFrame(frameSequence);
    for (const RhiSemanticEvent &event : frame.events)
    {
        if (const auto *observed = std::get_if<RhiObservedDraw>(&event.payload))
        {
            ++g_reportTotals.drawKinds[static_cast<std::size_t>(observed->state.draw.kind)];
            if (ExpectsBoundVertexState(observed->state.draw.kind))
            {
                if (observed->state.vertexStreams.empty())
                    ++g_reportTotals.boundDrawsWithoutVertexStreams;
                else
                    ++g_reportTotals.boundDrawsWithVertexStreams;
            }
            if (observed->state.renderTargets.empty())
                ++g_reportTotals.drawsWithoutRenderTargets;
            else
                ++g_reportTotals.drawsWithRenderTargets;
            continue;
        }
        if (const auto *observed = std::get_if<RhiObservedBinding>(&event.payload))
        {
            ++g_reportTotals.bindingKinds[static_cast<std::size_t>(observed->binding.kind)];
            continue;
        }
        if (const auto *observed = std::get_if<RhiObservedResourceLifetime>(&event.payload))
        {
            if (observed->lifetime.operation == RhiResourceLifetimeOperation::AddReference)
                ++g_reportTotals.resourceAddReferences;
            else
                ++g_reportTotals.resourceReleases;
            ++g_reportTotals.resourceTypes[observed->lifetime.resourceType & 0xF];
            continue;
        }
    }
    g_reportTotals.draws += frame.draws;
    g_reportTotals.matched += frame.matched;
    g_reportTotals.missing += frame.missing;
    g_reportTotals.mismatched += frame.mismatched;
    g_reportTotals.bindings += frame.bindings;
    g_reportTotals.bindingsMatched += frame.bindingsMatched;
    g_reportTotals.bindingsMissing += frame.bindingsMissing;
    g_reportTotals.bindingsMismatched += frame.bindingsMismatched;
    g_reportTotals.resourceLifetimeCalls += frame.resourceLifetimeCalls;
    g_reportTotals.resourceLifetimeMatched += frame.resourceLifetimeMatched;
    g_reportTotals.resourceLifetimeMissing += frame.resourceLifetimeMissing;
    g_reportTotals.resourceLifetimeMismatched += frame.resourceLifetimeMismatched;
    g_reportTotals.resourceRetirements += frame.resourceRetirements;
    g_reportTotals.resourceConstructions += frame.resourceConstructions;
    g_reportTotals.vertexStreamResets += frame.vertexStreamResets;
    g_reportTotals.vertexStreamResetsMatched += frame.vertexStreamResetsMatched;
    g_reportTotals.vertexStreamResetsMissing += frame.vertexStreamResetsMissing;
    g_reportTotals.vertexStreamResetsMismatched += frame.vertexStreamResetsMismatched;
    g_reportTotals.colorWriteStates += frame.colorWriteStates;
    g_reportTotals.colorWriteStatesMatched += frame.colorWriteStatesMatched;
    g_reportTotals.colorWriteStatesMissing += frame.colorWriteStatesMissing;
    g_reportTotals.colorWriteStatesMismatched += frame.colorWriteStatesMismatched;
    g_reportTotals.resolves += frame.resolves;
    g_reportTotals.resolvesMatched += frame.resolvesMatched;
    g_reportTotals.resolvesMissing += frame.resolvesMissing;
    g_reportTotals.resolvesMismatched += frame.resolvesMismatched;
    g_reportTotals.presents += frame.presents;
    g_reportTotals.presentsMatched += frame.presentsMatched;
    g_reportTotals.presentsMissing += frame.presentsMissing;
    g_reportTotals.presentsMismatched += frame.presentsMismatched;
    if (frame.frameSequence == 1 || frame.frameSequence % 60 == 0 || frame.missing != 0 ||
        frame.mismatched != 0 || frame.bindingsMissing != 0 || frame.bindingsMismatched != 0 ||
        frame.resourceLifetimeMissing != 0 || frame.resourceLifetimeMismatched != 0 ||
        frame.vertexStreamResetsMissing != 0 || frame.vertexStreamResetsMismatched != 0 ||
        frame.colorWriteStatesMissing != 0 || frame.colorWriteStatesMismatched != 0 ||
        frame.resolvesMissing != 0 || frame.resolvesMismatched != 0 || frame.presentsMissing != 0 ||
        frame.presentsMismatched != 0)
    {
        lucent::Line line;
        line.add("native RHI semantic through frame {}: {} draw call(s), {} packet match(es),"
                 " {} missing packet(s), {} mismatch(es); {} binding call(s), {} state"
                 " match(es), {} missing state observation(s), {} mismatch(es); {} resource"
                 " lifetime call(s), {} match(es), {} missing result(s), {} mismatch(es), {}"
                 " retirement(s) ({} add-reference, {} release), {} construction(s); {}"
                 " vertex-stream reset(s),"
                 " {} match(es), {} missing state observation(s), {} mismatch(es); {} color-write"
                 " state transition(s), {} match(es), {} missing active target(s), {}"
                 " mismatch(es); {} resolve"
                 " call(s), {} packet match(es), {} missing packet(s), {} mismatch(es);"
                 " {} present call(s),"
                 " {} packet match(es), {} missing packet(s), {} mismatch(es)",
                 frame.frameSequence, g_reportTotals.draws, g_reportTotals.matched,
                 g_reportTotals.missing, g_reportTotals.mismatched, g_reportTotals.bindings,
                 g_reportTotals.bindingsMatched, g_reportTotals.bindingsMissing,
                 g_reportTotals.bindingsMismatched, g_reportTotals.resourceLifetimeCalls,
                 g_reportTotals.resourceLifetimeMatched, g_reportTotals.resourceLifetimeMissing,
                 g_reportTotals.resourceLifetimeMismatched, g_reportTotals.resourceRetirements,
                 g_reportTotals.resourceAddReferences, g_reportTotals.resourceReleases,
                 g_reportTotals.resourceConstructions, g_reportTotals.vertexStreamResets,
                 g_reportTotals.vertexStreamResetsMatched, g_reportTotals.vertexStreamResetsMissing,
                 g_reportTotals.vertexStreamResetsMismatched, g_reportTotals.colorWriteStates,
                 g_reportTotals.colorWriteStatesMatched, g_reportTotals.colorWriteStatesMissing,
                 g_reportTotals.colorWriteStatesMismatched, g_reportTotals.resolves,
                 g_reportTotals.resolvesMatched, g_reportTotals.resolvesMissing,
                 g_reportTotals.resolvesMismatched, g_reportTotals.presents,
                 g_reportTotals.presentsMatched, g_reportTotals.presentsMissing,
                 g_reportTotals.presentsMismatched);
        for (std::size_t index = 0; index < g_reportTotals.drawKinds.size(); ++index)
        {
            if (g_reportTotals.drawKinds[index] != 0)
                line.add("  {} x{}", DrawKindName(static_cast<RhiSemanticDrawKind>(index)),
                         g_reportTotals.drawKinds[index]);
        }
        for (std::size_t index = 0; index < g_reportTotals.bindingKinds.size(); ++index)
        {
            if (g_reportTotals.bindingKinds[index] != 0)
                line.add("  {} x{}", BindingKindName(static_cast<RhiSemanticBindingKind>(index)),
                         g_reportTotals.bindingKinds[index]);
        }
        for (std::size_t index = 0; index < g_reportTotals.resourceTypes.size(); ++index)
        {
            if (g_reportTotals.resourceTypes[index] != 0)
                line.add("  resource-type-{} x{}", index, g_reportTotals.resourceTypes[index]);
        }
        line.add("  bound-draw-state with-streams={} without-streams={}",
                 g_reportTotals.boundDrawsWithVertexStreams,
                 g_reportTotals.boundDrawsWithoutVertexStreams);
        line.add("  draw-target-state with-targets={} without-targets={}",
                 g_reportTotals.drawsWithRenderTargets, g_reportTotals.drawsWithoutRenderTargets);
        line.flush_debug("rhi");
    }

    for (const RhiSemanticEvent &event : frame.events)
    {
        if (const auto *observed = std::get_if<RhiObservedDraw>(&event.payload))
        {
            if (observed->evidence != RhiDrawEvidenceResult::Match)
            {
                const RhiSemanticVertexStream &expected0 =
                    VertexStreamOrEmpty(observed->state.vertexStreams, 0);
                const RhiSemanticVertexStream &expected1 =
                    VertexStreamOrEmpty(observed->state.vertexStreams, 1);
                const RhiSemanticVertexStream &actual0 =
                    VertexStreamOrEmpty(observed->packet.vertexStreams, 0);
                const RhiSemanticVertexStream &actual1 =
                    VertexStreamOrEmpty(observed->packet.vertexStreams, 1);
                const RhiSemanticRenderTarget &expectedTarget =
                    RenderTargetOrEmpty(observed->state.renderTargets, 0);
                const RhiSemanticRenderTarget &actualTarget =
                    RenderTargetOrEmpty(observed->packet.renderTargets, 0);
                lucent::error(
                    "rhi",
                    "semantic draw {} ({}) did not match its guest PM4 emission:"
                    " expected prim {:#x} count {}, observed present={} opcode={:#x}"
                    " prim={:#x} source={} count={}; expected vertex={:#x}+{}"
                    " index={:#x}+{}, observed resources present={} vertex={:#x}+{}"
                    " index={:#x}+{}; bound index view present={} base={:#x}+{} stride={}"
                    " endian={}, observed DMA present={} index={:#x}+{} stride={} endian={};"
                    " expected active vertex streams={} first=({},{:#x}) second=({},{:#x}),"
                    " observed present={} streams={} first=({},{:#x}) second=({},{:#x});"
                    " expected targets={} first=(depth={} slot={} object={:#x}), observed"
                    " present={} targets={} first=(depth={} slot={} object={:#x})",
                    event.sequence, DrawKindName(observed->state.draw.kind),
                    observed->state.draw.primitiveType, observed->state.draw.elementCount,
                    observed->packet.present, observed->packet.opcode,
                    observed->packet.primitiveType, observed->packet.sourceSelect,
                    observed->packet.elementCount, observed->state.draw.vertexData.guestAddress,
                    observed->state.draw.vertexData.sizeBytes,
                    observed->state.draw.indexData.guestAddress,
                    observed->state.draw.indexData.sizeBytes, observed->packet.transientDataPresent,
                    observed->packet.vertexData.guestAddress, observed->packet.vertexData.sizeBytes,
                    observed->packet.indexData.guestAddress, observed->packet.indexData.sizeBytes,
                    observed->state.draw.indexBufferViewPresent,
                    observed->state.draw.indexBuffer.allocation.guestAddress,
                    observed->state.draw.indexBuffer.allocation.sizeBytes,
                    observed->state.draw.indexBuffer.elementStrideBytes,
                    observed->state.draw.indexBuffer.endianSwap, observed->packet.indexDataPresent,
                    observed->packet.indexData.guestAddress, observed->packet.indexData.sizeBytes,
                    observed->packet.indexStrideBytes, observed->packet.indexEndianSwap,
                    observed->state.vertexStreams.size(), expected0.slot, expected0.object,
                    expected1.slot, expected1.object, observed->packet.vertexStreamsPresent,
                    observed->packet.vertexStreams.size(), actual0.slot, actual0.object,
                    actual1.slot, actual1.object, observed->state.renderTargets.size(),
                    expectedTarget.depthStencil, expectedTarget.slot, expectedTarget.object,
                    observed->packet.renderTargetsPresent, observed->packet.renderTargets.size(),
                    actualTarget.depthStencil, actualTarget.slot, actualTarget.object);
            }
            continue;
        }

        if (const auto *observed = std::get_if<RhiObservedBinding>(&event.payload))
        {
            if (observed->evidence != RhiBindingEvidenceResult::Match)
            {
                lucent::error(
                    "rhi",
                    "semantic binding {} ({} slot {}) did not match guest device state:"
                    " expected object {:#x} descriptor {}:{:#x}, observed present={}"
                    " object={:#x} descriptor {}:{:#x}; expected buffer view={}"
                    " {:#x}+{} stride={} endian={}, observed buffer view={}"
                    " {:#x}+{} stride={} endian={}",
                    event.sequence, BindingKindName(observed->binding.kind), observed->binding.slot,
                    observed->binding.object, observed->binding.descriptorDwords,
                    observed->binding.descriptor[0], observed->state.present,
                    observed->state.observedObject, observed->state.descriptorDwords,
                    observed->state.descriptor[0], observed->binding.bufferViewPresent,
                    observed->binding.bufferView.allocation.guestAddress,
                    observed->binding.bufferView.allocation.sizeBytes,
                    observed->binding.bufferView.elementStrideBytes,
                    observed->binding.bufferView.endianSwap, observed->state.bufferViewPresent,
                    observed->state.bufferView.allocation.guestAddress,
                    observed->state.bufferView.allocation.sizeBytes,
                    observed->state.bufferView.elementStrideBytes,
                    observed->state.bufferView.endianSwap);
            }
            continue;
        }

        if (const auto *observed = std::get_if<RhiObservedResourceLifetime>(&event.payload))
        {
            if (observed->evidence != RhiResourceLifetimeEvidenceResult::Match)
            {
                lucent::error(
                    "rhi",
                    "semantic resource lifetime {} ({} object {:#x}, type {}, flags {:#x},"
                    " backing {:#x}) did not match retained refcount arithmetic: previous={}"
                    " observed present={} returned={}",
                    event.sequence,
                    observed->lifetime.operation == RhiResourceLifetimeOperation::AddReference
                        ? "add-reference"
                        : "release",
                    observed->lifetime.object, observed->lifetime.resourceType,
                    observed->lifetime.rawFlags, observed->lifetime.backingObject,
                    observed->lifetime.previousReferenceCount, observed->retained.present,
                    observed->retained.returnedReferenceCount);
            }
            continue;
        }

        if (const auto *observed = std::get_if<RhiObservedResourceConstruction>(&event.payload))
        {
            const char *kind =
                observed->construction.kind == RhiSemanticResourceConstructionKind::OwnedBacking
                    ? "owned-backing"
                    : "wrapped-backing";
            lucent::debug(
                "rhi",
                "resource construction {} ({}) requested={} flags={:#x} allocation-flags={:#x}"
                " present={} object={:#x} words={:#x},{:#x},{:#x},{:#x},{:#x}",
                event.sequence, kind, observed->construction.requestedBytes,
                observed->construction.resourceFlags, observed->construction.allocationFlags,
                observed->retained.present, observed->retained.object,
                observed->retained.objectWords[0], observed->retained.objectWords[1],
                observed->retained.objectWords[2], observed->retained.objectWords[3],
                observed->retained.objectWords[4]);
            continue;
        }

        if (const auto *observed = std::get_if<RhiObservedVertexStreamReset>(&event.payload))
        {
            if (observed->evidence != RhiVertexStreamResetEvidenceResult::Match)
            {
                lucent::error(
                    "rhi",
                    "semantic vertex-stream reset {} (slots {}..{}) did not match guest device"
                    " state: observed present={} active streams={}",
                    event.sequence, observed->reset.firstSlot,
                    observed->reset.firstSlot + observed->reset.slotCount, observed->state.present,
                    observed->state.activeStreams.size());
            }
            continue;
        }

        if (const auto *observed = std::get_if<RhiObservedColorWriteState>(&event.payload))
        {
            if (observed->evidence != RhiColorWriteStateEvidenceResult::Match)
            {
                lucent::error(
                    "rhi",
                    "semantic color-write state {} (requested {:#x}) did not match the active"
                    " target: target present={} slot={} object={:#x} descriptor present={}"
                    " base={:#x} format={} exponent={}; result={}",
                    event.sequence, observed->state.requested, observed->state.targetPresent,
                    observed->state.target.slot, observed->state.target.object,
                    observed->state.target.normalizedStatePresent,
                    observed->state.target.normalizedState.base,
                    observed->state.target.normalizedState.format,
                    observed->state.target.normalizedState.colorExponentBias,
                    observed->evidence == RhiColorWriteStateEvidenceResult::Missing ? "missing"
                                                                                    : "mismatch");
            }
            continue;
        }
        if (std::holds_alternative<RhiObservedViewport>(event.payload))
            continue;
        if (const auto *observed = std::get_if<RhiObservedResolve>(&event.payload))
        {
            if (observed->evidence != RhiResolveEvidenceResult::Match)
            {
                lucent::error(
                    "rhi",
                    "semantic resolve {} ({} source slot {}) did not match its guest kCopy:"
                    " expected source object {:#x}, destination object {:#x} at {:#x}"
                    " pitch={} height={} format={}; observed present={} source object={:#x}"
                    " destination={:#x} pitch={} height={} opcode={:#x} primitive={}"
                    " source={} count={}",
                    event.sequence, observed->resolve.sourceDepthStencil ? "depth" : "color",
                    observed->resolve.sourceSlot, observed->resolve.sourceObject,
                    observed->resolve.destinationObject, observed->resolve.destinationAddress,
                    observed->resolve.destinationPitch, observed->resolve.destinationHeight,
                    observed->resolve.destinationFormat, observed->packet.present,
                    observed->packet.observedSourceObject, observed->packet.destinationAddress,
                    observed->packet.destinationPitch, observed->packet.destinationHeight,
                    observed->packet.drawOpcode, observed->packet.primitiveType,
                    observed->packet.sourceSelect, observed->packet.elementCount);
            }
            continue;
        }

        const auto &observed = std::get<RhiObservedPresent>(event.payload);
        if (observed.evidence == RhiPresentEvidenceResult::Match)
            continue;
        lucent::error(
            "rhi",
            "semantic present {} (frame {}) did not match its host swap packet:"
            " expected front buffer {:#x}, observed present={} frame={} front buffer={:#x}",
            event.sequence, observed.present.frameSequence, observed.present.frontBuffer,
            observed.packet.present, observed.packet.frameSequence, observed.packet.frontBuffer);
    }

    return frame;
}

} // namespace gears
