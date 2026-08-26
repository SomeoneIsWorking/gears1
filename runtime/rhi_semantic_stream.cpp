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
    std::vector<RhiObservedDraw> pending;
    std::uint64_t nextSequence = 1;
};

RhiSemanticStreamState g_stream;

struct RhiSemanticReportTotals
{
    std::uint64_t draws = 0;
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t mismatched = 0;
    std::array<std::uint64_t, 4> kinds{};
};

RhiSemanticReportTotals g_reportTotals;

[[nodiscard]] bool ExpectsDmaIndices(RhiSemanticDrawKind kind)
{
    return kind == RhiSemanticDrawKind::TransientVerticesAndIndices ||
           kind == RhiSemanticDrawKind::BoundIndices;
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
    return RhiDrawEvidenceResult::Match;
}

void ObserveRhiSemanticDraw(const RhiSemanticDraw &draw, const RhiDrawPacketEvidence &packet)
{
    if (!RhiSemanticObservationEnabled())
        return;

    std::lock_guard guard(g_stream.mutex);
    g_stream.pending.push_back({.sequence = g_stream.nextSequence++,
                                .draw = draw,
                                .packet = packet,
                                .evidence = CompareRhiDrawPacket(draw, packet)});
}

RhiSemanticFrame SealRhiSemanticFrame(std::uint64_t frameSequence)
{
    RhiSemanticFrame frame{.frameSequence = frameSequence};
    {
        std::lock_guard guard(g_stream.mutex);
        frame.draws = std::move(g_stream.pending);
        g_stream.pending.clear();
    }
    for (const RhiObservedDraw &observed : frame.draws)
    {
        switch (observed.evidence)
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
    }
    return frame;
}

void ReportRhiSemanticFrame(std::uint64_t frameSequence)
{
    if (!RhiSemanticObservationEnabled())
        return;

    const RhiSemanticFrame frame = SealRhiSemanticFrame(frameSequence);
    for (const RhiObservedDraw &observed : frame.draws)
        ++g_reportTotals.kinds[static_cast<std::size_t>(observed.draw.kind)];
    g_reportTotals.draws += frame.draws.size();
    g_reportTotals.matched += frame.matched;
    g_reportTotals.missing += frame.missing;
    g_reportTotals.mismatched += frame.mismatched;

    if (frame.frameSequence == 1 || frame.frameSequence % 60 == 0 || frame.missing != 0 ||
        frame.mismatched != 0)
    {
        lucent::Line line;
        line.add("native RHI semantic through frame {}: {} draw call(s), {} packet match(es),"
                 " {} missing packet(s), {} mismatch(es)",
                 frame.frameSequence, g_reportTotals.draws, g_reportTotals.matched,
                 g_reportTotals.missing, g_reportTotals.mismatched);
        for (std::size_t index = 0; index < g_reportTotals.kinds.size(); ++index)
        {
            if (g_reportTotals.kinds[index] != 0)
                line.add("  {} x{}", DrawKindName(static_cast<RhiSemanticDrawKind>(index)),
                         g_reportTotals.kinds[index]);
        }
        line.flush_debug("rhi");
    }

    for (const RhiObservedDraw &observed : frame.draws)
    {
        if (observed.evidence == RhiDrawEvidenceResult::Match)
            continue;
        lucent::error("rhi",
                      "semantic draw {} ({}) did not match its guest PM4 emission:"
                      " expected prim {:#x} count {}, observed present={} opcode={:#x}"
                      " prim={:#x} source={} count={}",
                      observed.sequence, DrawKindName(observed.draw.kind),
                      observed.draw.primitiveType, observed.draw.elementCount,
                      observed.packet.present, observed.packet.opcode,
                      observed.packet.primitiveType, observed.packet.sourceSelect,
                      observed.packet.elementCount);
    }
}

} // namespace gears
