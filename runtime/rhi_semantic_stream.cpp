#include "rhi_semantic_stream.h"

#include <array>
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
};

RhiSemanticStreamState g_stream;

struct RhiSemanticReportTotals
{
    std::uint64_t draws = 0;
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t mismatched = 0;
    std::array<std::uint64_t, 4> drawKinds{};
    std::uint64_t bindings = 0;
    std::uint64_t bindingsMatched = 0;
    std::uint64_t bindingsMissing = 0;
    std::uint64_t bindingsMismatched = 0;
    std::array<std::uint64_t, 5> bindingKinds{};
    std::uint64_t presents = 0;
    std::uint64_t presentsMatched = 0;
    std::uint64_t presentsMissing = 0;
    std::uint64_t presentsMismatched = 0;
};

RhiSemanticReportTotals g_reportTotals;

[[nodiscard]] bool ExpectsDmaIndices(RhiSemanticDrawKind kind)
{
    return kind == RhiSemanticDrawKind::TransientVerticesAndIndices ||
           kind == RhiSemanticDrawKind::BoundIndices;
}

[[nodiscard]] bool ExpectsTransientData(RhiSemanticDrawKind kind)
{
    return kind == RhiSemanticDrawKind::TransientVertices ||
           kind == RhiSemanticDrawKind::TransientVerticesAndIndices;
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
    case RhiSemanticBindingKind::PixelShader:
        return "pixel-shader";
    case RhiSemanticBindingKind::VertexShader:
        return "vertex-shader";
    case RhiSemanticBindingKind::IndexBuffer:
        return "index-buffer";
    case RhiSemanticBindingKind::VertexStream:
        return "vertex-stream";
    }
    return "unknown";
}

} // namespace

bool RhiSemanticObservationEnabled()
{
    static const bool enabled = lucent::config::flag("NATIVE_RHI_OBSERVE");
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
    const std::uint32_t expectedSource = ExpectsDmaIndices(draw.kind) ? 0u : 2u;
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

void ObserveRhiSemanticDraw(const RhiSemanticDraw &draw, const RhiDrawPacketEvidence &packet)
{
    if (!RhiSemanticObservationEnabled())
        return;

    std::lock_guard guard(g_stream.mutex);
    g_stream.pendingEvents.push_back(
        {.sequence = g_stream.nextSequence++,
         .payload = RhiObservedDraw{
             .draw = draw, .packet = packet, .evidence = CompareRhiDrawPacket(draw, packet)}});
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
    return frame;
}

void ReportRhiSemanticFrame(std::uint64_t frameSequence)
{
    if (!RhiSemanticObservationEnabled())
        return;

    const RhiSemanticFrame frame = SealRhiSemanticFrame(frameSequence);
    for (const RhiSemanticEvent &event : frame.events)
    {
        if (const auto *observed = std::get_if<RhiObservedDraw>(&event.payload))
        {
            ++g_reportTotals.drawKinds[static_cast<std::size_t>(observed->draw.kind)];
            continue;
        }
        if (const auto *observed = std::get_if<RhiObservedBinding>(&event.payload))
        {
            ++g_reportTotals.bindingKinds[static_cast<std::size_t>(observed->binding.kind)];
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
    g_reportTotals.presents += frame.presents;
    g_reportTotals.presentsMatched += frame.presentsMatched;
    g_reportTotals.presentsMissing += frame.presentsMissing;
    g_reportTotals.presentsMismatched += frame.presentsMismatched;

    if (frame.frameSequence == 1 || frame.frameSequence % 60 == 0 || frame.missing != 0 ||
        frame.mismatched != 0 || frame.bindingsMissing != 0 || frame.bindingsMismatched != 0 ||
        frame.presentsMissing != 0 || frame.presentsMismatched != 0)
    {
        lucent::Line line;
        line.add("native RHI semantic through frame {}: {} draw call(s), {} packet match(es),"
                 " {} missing packet(s), {} mismatch(es); {} binding call(s), {} state"
                 " match(es), {} missing state observation(s), {} mismatch(es); {} present call(s),"
                 " {} packet match(es), {} missing packet(s), {} mismatch(es)",
                 frame.frameSequence, g_reportTotals.draws, g_reportTotals.matched,
                 g_reportTotals.missing, g_reportTotals.mismatched, g_reportTotals.bindings,
                 g_reportTotals.bindingsMatched, g_reportTotals.bindingsMissing,
                 g_reportTotals.bindingsMismatched, g_reportTotals.presents,
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
        line.flush_debug("rhi");
    }

    for (const RhiSemanticEvent &event : frame.events)
    {
        if (const auto *observed = std::get_if<RhiObservedDraw>(&event.payload))
        {
            if (observed->evidence != RhiDrawEvidenceResult::Match)
            {
                lucent::error(
                    "rhi",
                    "semantic draw {} ({}) did not match its guest PM4 emission:"
                    " expected prim {:#x} count {}, observed present={} opcode={:#x}"
                    " prim={:#x} source={} count={}; expected vertex={:#x}+{}"
                    " index={:#x}+{}, observed resources present={} vertex={:#x}+{}"
                    " index={:#x}+{}; bound index view present={} base={:#x}+{} stride={}"
                    " endian={}, observed DMA present={} index={:#x}+{} stride={} endian={}",
                    event.sequence, DrawKindName(observed->draw.kind), observed->draw.primitiveType,
                    observed->draw.elementCount, observed->packet.present, observed->packet.opcode,
                    observed->packet.primitiveType, observed->packet.sourceSelect,
                    observed->packet.elementCount, observed->draw.vertexData.guestAddress,
                    observed->draw.vertexData.sizeBytes, observed->draw.indexData.guestAddress,
                    observed->draw.indexData.sizeBytes, observed->packet.transientDataPresent,
                    observed->packet.vertexData.guestAddress, observed->packet.vertexData.sizeBytes,
                    observed->packet.indexData.guestAddress, observed->packet.indexData.sizeBytes,
                    observed->draw.indexBufferViewPresent,
                    observed->draw.indexBuffer.allocation.guestAddress,
                    observed->draw.indexBuffer.allocation.sizeBytes,
                    observed->draw.indexBuffer.elementStrideBytes,
                    observed->draw.indexBuffer.endianSwap, observed->packet.indexDataPresent,
                    observed->packet.indexData.guestAddress, observed->packet.indexData.sizeBytes,
                    observed->packet.indexStrideBytes, observed->packet.indexEndianSwap);
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
}

} // namespace gears
