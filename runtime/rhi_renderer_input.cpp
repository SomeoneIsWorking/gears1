#include "rhi_renderer_input.h"

#include "guest_address.h"
#include "gpu_draw.h"
#include "rhi_semantic_stream.h"

#include <algorithm>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include <lucent/log.h>

namespace gears
{
namespace
{

struct RendererFrame
{
    bool valid = true;
    RhiRendererFrameInput input;
};

struct SemanticFrame
{
    bool valid = true;
    RhiSemanticFrame input;
};

struct RendererJoinState
{
    std::mutex mutex;
    std::map<std::uint64_t, RendererFrame> rendererFrames;
    std::map<std::uint64_t, SemanticFrame> semanticFrames;
    std::map<std::uint64_t, RhiRendererFrameComparison> completedFrames;
    std::uint64_t retiredCompletedThrough = 0;
    std::uint64_t highestSequence = 0;
};

struct RendererReportTotals
{
    std::uint64_t frames = 0;
    std::uint64_t unavailableFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t sourceDraws = 0;
    std::uint64_t materializedDraws = 0;
    std::uint64_t refusedDraws = 0;
    std::uint64_t resolveDraws = 0;
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t unmatchedRendererPackets = 0;
    std::uint64_t unmatchedRendererMaterializedPackets = 0;
    std::uint64_t unmatchedRendererRefusedPackets = 0;
    std::uint64_t unmatchedRendererMixedOutcomePackets = 0;
    std::uint64_t unmatchedRendererIndirectPackets = 0;
    std::uint64_t unmatchedRendererRingPackets = 0;
    std::uint64_t unmatchedRendererInconsistentSourcePackets = 0;
    std::uint64_t unkeyedRendererDraws = 0;
    std::uint64_t duplicateFrames = 0;
    bool reportedFailure = false;
};

struct PendingReport
{
    std::uint64_t frameSequence = 0;
    RhiRendererFrameComparison comparison;
};

RendererJoinState g_join;
RendererReportTotals g_reportTotals;
std::mutex g_reportMutex;
// The live renderer queue holds at most two frames. This larger sequence
// window turns a missing terminal callback into explicit evidence without
// allowing either side of the asynchronous join to grow for a whole run.
constexpr std::size_t kRendererJoinHistory = 64;

[[nodiscard]] const char *FrameStatusName(draw::NativeFrameMaterializationStatus status)
{
    switch (status)
    {
    case draw::NativeFrameMaterializationStatus::Complete:
        return "complete";
    case draw::NativeFrameMaterializationStatus::RendererUnavailable:
        return "renderer-unavailable";
    case draw::NativeFrameMaterializationStatus::Dropped:
        return "dropped";
    }
    return "unknown";
}

[[nodiscard]] const char *DrawOutcomeName(draw::NativeDrawMaterializationOutcome outcome)
{
    switch (outcome)
    {
    case draw::NativeDrawMaterializationOutcome::Refused:
        return "refused";
    case draw::NativeDrawMaterializationOutcome::Resolve:
        return "resolve";
    case draw::NativeDrawMaterializationOutcome::Materialized:
        return "materialized";
    }
    return "unknown";
}

[[nodiscard]] const char *EvidenceReasonName(RhiRendererDrawEvidenceReason reason)
{
    switch (reason)
    {
    case RhiRendererDrawEvidenceReason::None:
        return "none";
    case RhiRendererDrawEvidenceReason::RendererRefused:
        return "renderer-refused";
    case RhiRendererDrawEvidenceReason::DrawShape:
        return "draw-shape";
    case RhiRendererDrawEvidenceReason::IndexBufferViewMissing:
        return "index-buffer-view-missing";
    case RhiRendererDrawEvidenceReason::IndexWidth:
        return "index-width";
    case RhiRendererDrawEvidenceReason::IndexAddress:
        return "index-address";
    case RhiRendererDrawEvidenceReason::IndexEndian:
        return "index-endian";
    case RhiRendererDrawEvidenceReason::DuplicateColorTarget:
        return "duplicate-color-target";
    case RhiRendererDrawEvidenceReason::DuplicateDepthTarget:
        return "duplicate-depth-target";
    case RhiRendererDrawEvidenceReason::UnsupportedColorTargetSlot:
        return "unsupported-color-target-slot";
    case RhiRendererDrawEvidenceReason::RendererTargetStateMissing:
        return "renderer-target-state-missing";
    case RhiRendererDrawEvidenceReason::SemanticSurfaceStateMissing:
        return "semantic-surface-state-missing";
    case RhiRendererDrawEvidenceReason::ColorTargetStateUnavailable:
        return "color-target-state-unavailable";
    case RhiRendererDrawEvidenceReason::DepthTargetStateUnavailable:
        return "depth-target-state-unavailable";
    case RhiRendererDrawEvidenceReason::ColorTargetStateMissing:
        return "color-target-state-missing";
    case RhiRendererDrawEvidenceReason::DepthTargetStateMissing:
        return "depth-target-state-missing";
    case RhiRendererDrawEvidenceReason::SurfaceState:
        return "surface-state";
    case RhiRendererDrawEvidenceReason::ColorTargetState:
        return "color-target-state";
    case RhiRendererDrawEvidenceReason::DepthTargetState:
        return "depth-target-state";
    }
    return "unknown";
}

[[nodiscard]] std::uint32_t CanonicalPacketAddress(std::uint32_t guestAddress)
{
    return guestAddress & kGuestPhysicalAddressMask & ~std::uint32_t{3};
}

[[nodiscard]] std::string
DescribeFirstUnmatchedRendererPacket(const RhiRendererFrameComparison &comparison)
{
    if (comparison.unmatchedRendererPackets == 0)
        return "none";
    const char *outcome = comparison.firstUnmatchedRendererMixedOutcome
                              ? "mixed outcome"
                              : DrawOutcomeName(comparison.firstUnmatchedRendererOutcome);
    if (comparison.firstUnmatchedRendererInconsistentSource)
        return std::format("{:#x} with source conflict ({})",
                           comparison.firstUnmatchedRendererPacket, outcome);
    return std::format("{:#x} from {} buffer {:#x} ({})", comparison.firstUnmatchedRendererPacket,
                       comparison.firstUnmatchedRendererFromIndirectBuffer ? "indirect" : "ring",
                       comparison.firstUnmatchedRendererBuffer, outcome);
}

void ReportComparison(std::uint64_t frameSequence, const RhiRendererFrameComparison &comparison)
{
    std::lock_guard guard(g_reportMutex);
    ++g_reportTotals.frames;
    if (comparison.status == draw::NativeFrameMaterializationStatus::RendererUnavailable)
        ++g_reportTotals.unavailableFrames;
    if (comparison.status == draw::NativeFrameMaterializationStatus::Dropped)
        ++g_reportTotals.droppedFrames;
    g_reportTotals.sourceDraws += comparison.sourceDraws;
    g_reportTotals.materializedDraws += comparison.materializedDraws;
    g_reportTotals.refusedDraws += comparison.refusedDraws;
    g_reportTotals.resolveDraws += comparison.resolveDraws;
    g_reportTotals.matched += comparison.matched;
    g_reportTotals.missing += comparison.missing;
    g_reportTotals.mismatched += comparison.mismatched;
    g_reportTotals.unmatchedRendererPackets += comparison.unmatchedRendererPackets;
    g_reportTotals.unmatchedRendererMaterializedPackets +=
        comparison.unmatchedRendererMaterializedPackets;
    g_reportTotals.unmatchedRendererRefusedPackets += comparison.unmatchedRendererRefusedPackets;
    g_reportTotals.unmatchedRendererMixedOutcomePackets +=
        comparison.unmatchedRendererMixedOutcomePackets;
    g_reportTotals.unmatchedRendererIndirectPackets += comparison.unmatchedRendererIndirectPackets;
    g_reportTotals.unmatchedRendererRingPackets += comparison.unmatchedRendererRingPackets;
    g_reportTotals.unmatchedRendererInconsistentSourcePackets +=
        comparison.unmatchedRendererInconsistentSourcePackets;
    g_reportTotals.unkeyedRendererDraws += comparison.unkeyedRendererDraws;
    if (comparison.duplicate)
        ++g_reportTotals.duplicateFrames;

    const bool failed = comparison.missing != 0 || comparison.mismatched != 0 ||
                        comparison.unmatchedRendererPackets != 0 ||
                        comparison.unkeyedRendererDraws != 0 || comparison.duplicate;
    const bool firstFailure = failed && !g_reportTotals.reportedFailure;
    g_reportTotals.reportedFailure = g_reportTotals.reportedFailure || failed;
    if (frameSequence == 1 || frameSequence % 60 == 0 || firstFailure ||
        comparison.unmatchedRendererPackets != 0 || comparison.duplicate ||
        comparison.status == draw::NativeFrameMaterializationStatus::RendererUnavailable)
    {
        lucent::info(
            "rhi",
            "native RHI renderer materialization through frame {}: {} frame(s), {} unavailable,"
            " {} dropped; {} source draw(s), {} materialized, {} refused, {} resolve; {}"
            " semantic match(es), {} missing, {} mismatch(es), {} unmatched renderer"
            " packet(s) ({} materialized, {} refused, {} mixed outcome; {} indirect, {} ring,"
            " {} source conflict), {} unkeyed"
            " renderer draw(s), {} duplicate frame(s); current frame"
            " {}{}; first missing semantic packet {:#x}, first mismatched semantic packet"
            " {:#x} ({}), first"
            " unmatched renderer packet {}",
            frameSequence, g_reportTotals.frames, g_reportTotals.unavailableFrames,
            g_reportTotals.droppedFrames, g_reportTotals.sourceDraws,
            g_reportTotals.materializedDraws, g_reportTotals.refusedDraws,
            g_reportTotals.resolveDraws, g_reportTotals.matched, g_reportTotals.missing,
            g_reportTotals.mismatched, g_reportTotals.unmatchedRendererPackets,
            g_reportTotals.unmatchedRendererMaterializedPackets,
            g_reportTotals.unmatchedRendererRefusedPackets,
            g_reportTotals.unmatchedRendererMixedOutcomePackets,
            g_reportTotals.unmatchedRendererIndirectPackets,
            g_reportTotals.unmatchedRendererRingPackets,
            g_reportTotals.unmatchedRendererInconsistentSourcePackets,
            g_reportTotals.unkeyedRendererDraws, g_reportTotals.duplicateFrames,
            FrameStatusName(comparison.status), comparison.duplicate ? ", duplicate" : "",
            comparison.firstMissingSemanticPacket, comparison.firstMismatchedSemanticPacket,
            EvidenceReasonName(comparison.firstMismatchReason),
            DescribeFirstUnmatchedRendererPacket(comparison));
    }
}

void ReportLateDuplicate(std::uint64_t frameSequence)
{
    std::lock_guard guard(g_reportMutex);
    ++g_reportTotals.duplicateFrames;
    g_reportTotals.reportedFailure = true;
    lucent::error("rhi",
                  "frame {} was invalidated by a post-completion duplicate; {} duplicate"
                  " frame(s) observed",
                  frameSequence, g_reportTotals.duplicateFrames);
}

[[nodiscard]] RhiRendererFrameComparison
InvalidateCompletedComparison(const RhiRendererFrameComparison *completed)
{
    RhiRendererFrameComparison invalid;
    if (completed != nullptr)
    {
        invalid = *completed;
        invalid.matched = 0;
        invalid.missing = invalid.semanticDraws;
        invalid.mismatched = 0;
        invalid.unmatchedRendererPackets = 0;
        invalid.unkeyedRendererDraws = 0;
    }
    invalid.status = draw::NativeFrameMaterializationStatus::RendererUnavailable;
    invalid.duplicate = true;
    return invalid;
}

void RememberCompletedFrame(std::uint64_t frameSequence,
                            const RhiRendererFrameComparison &comparison)
{
    g_join.completedFrames.insert_or_assign(frameSequence, comparison);
    while (g_join.completedFrames.size() > kRendererJoinHistory)
    {
        g_join.retiredCompletedThrough =
            std::max(g_join.retiredCompletedThrough, g_join.completedFrames.begin()->first);
        g_join.completedFrames.erase(g_join.completedFrames.begin());
    }
}

[[nodiscard]] std::optional<RhiRendererFrameComparison>
TakeComparisonLocked(std::uint64_t frameSequence)
{
    const auto semantic = g_join.semanticFrames.find(frameSequence);
    const auto renderer = g_join.rendererFrames.find(frameSequence);
    if (semantic == g_join.semanticFrames.end() || renderer == g_join.rendererFrames.end())
        return std::nullopt;

    RhiRendererFrameComparison comparison =
        CompareRhiRendererDraws(semantic->second.input, renderer->second.input);
    comparison.duplicate = !semantic->second.valid || !renderer->second.valid;
    if (comparison.duplicate)
    {
        comparison.status = draw::NativeFrameMaterializationStatus::RendererUnavailable;
        comparison.missing = comparison.semanticDraws;
    }
    g_join.semanticFrames.erase(semantic);
    g_join.rendererFrames.erase(renderer);
    RememberCompletedFrame(frameSequence, comparison);
    return comparison;
}

[[nodiscard]] std::vector<PendingReport> RetireStaleJoinsLocked(std::uint64_t frameSequence)
{
    g_join.highestSequence = std::max(g_join.highestSequence, frameSequence);
    if (g_join.highestSequence <= kRendererJoinHistory)
        return {};

    const std::uint64_t oldestAllowed = g_join.highestSequence - kRendererJoinHistory;
    std::vector<PendingReport> reports;
    while (!g_join.semanticFrames.empty() && g_join.semanticFrames.begin()->first < oldestAllowed)
    {
        auto stale = g_join.semanticFrames.begin();
        const std::uint64_t sequence = stale->first;
        lucent::error("rhi", "renderer materialization never arrived for semantic frame {}",
                      sequence);
        reports.push_back(
            {sequence,
             CompareRhiRendererDraws(
                 stale->second.input,
                 {.status = draw::NativeFrameMaterializationStatus::RendererUnavailable})});
        g_join.semanticFrames.erase(stale);
        RememberCompletedFrame(sequence, reports.back().comparison);
    }
    while (!g_join.rendererFrames.empty() && g_join.rendererFrames.begin()->first < oldestAllowed)
    {
        auto stale = g_join.rendererFrames.begin();
        const std::uint64_t sequence = stale->first;
        lucent::error("rhi", "semantic frame never arrived for renderer materialization {}",
                      sequence);
        reports.push_back(
            {sequence, CompareRhiRendererDraws({.frameSequence = sequence}, stale->second.input)});
        g_join.rendererFrames.erase(stale);
        RememberCompletedFrame(sequence, reports.back().comparison);
    }
    return reports;
}

void ReportAll(const std::vector<PendingReport> &reports)
{
    for (const PendingReport &report : reports)
        ReportComparison(report.frameSequence, report.comparison);
}

} // namespace

RhiRendererDrawEvidence InspectRhiRendererDrawInput(const RhiSemanticDrawState &state,
                                                    const RhiRendererDrawInput &renderer)
{
    if (renderer.outcome != draw::NativeDrawMaterializationOutcome::Materialized)
        return {RhiRendererDrawEvidenceResult::Missing,
                RhiRendererDrawEvidenceReason::RendererRefused};
    const RhiSemanticDraw &draw = state.draw;
    const bool expectedIndexed = RhiDrawUsesDmaIndices(draw.kind);
    if (renderer.primitiveType != (draw.primitiveType & 0x3F) ||
        renderer.elementCount != draw.elementCount || renderer.indexed != expectedIndexed)
        return {RhiRendererDrawEvidenceResult::Mismatch, RhiRendererDrawEvidenceReason::DrawShape};
    if (expectedIndexed)
    {
        bool expected32 = false;
        std::uint32_t expectedEndian = 0;
        if (draw.kind == RhiSemanticDrawKind::BoundIndices)
        {
            if (!draw.indexBufferViewPresent)
                return {RhiRendererDrawEvidenceResult::Missing,
                        RhiRendererDrawEvidenceReason::IndexBufferViewMissing};
            expected32 = draw.indexBuffer.elementStrideBytes == 4;
            expectedEndian = draw.indexBuffer.endianSwap;
        }
        else
        {
            expected32 = (draw.indexFormatFlags & 4) != 0;
        }
        if (renderer.indexIs32 != expected32)
            return {RhiRendererDrawEvidenceResult::Mismatch,
                    RhiRendererDrawEvidenceReason::IndexWidth};
        if ((renderer.indexGuestBase & kGuestPhysicalAddressMask) !=
            (draw.indexData.guestAddress & kGuestPhysicalAddressMask))
            return {RhiRendererDrawEvidenceResult::Mismatch,
                    RhiRendererDrawEvidenceReason::IndexAddress};
        if (draw.kind == RhiSemanticDrawKind::BoundIndices &&
            renderer.indexEndian != expectedEndian)
        {
            return {RhiRendererDrawEvidenceResult::Mismatch,
                    RhiRendererDrawEvidenceReason::IndexEndian};
        }
    }

    bool targetParityExpected = state.surfaceStatePresent;
    const RhiSemanticRenderTarget *colorTarget = nullptr;
    const RhiSemanticRenderTarget *depthTarget = nullptr;
    for (const RhiSemanticRenderTarget &target : state.renderTargets)
    {
        targetParityExpected = targetParityExpected || target.normalizedStatePresent;
        if (target.depthStencil)
        {
            if (depthTarget != nullptr)
                return {RhiRendererDrawEvidenceResult::Mismatch,
                        RhiRendererDrawEvidenceReason::DuplicateDepthTarget};
            depthTarget = &target;
        }
        else if (target.slot == 0)
        {
            if (colorTarget != nullptr)
                return {RhiRendererDrawEvidenceResult::Mismatch,
                        RhiRendererDrawEvidenceReason::DuplicateColorTarget};
            colorTarget = &target;
        }
        else if (targetParityExpected)
        {
            // The compatibility renderer materializes RT0 only. A semantic
            // MRT binding is unsupported parity coverage, not an agreement.
            return {RhiRendererDrawEvidenceResult::Missing,
                    RhiRendererDrawEvidenceReason::UnsupportedColorTargetSlot};
        }
    }
    if (!targetParityExpected)
        return {RhiRendererDrawEvidenceResult::Match, RhiRendererDrawEvidenceReason::None};
    if (!renderer.targetStatePresent)
        return {RhiRendererDrawEvidenceResult::Missing,
                RhiRendererDrawEvidenceReason::RendererTargetStateMissing};
    if (!state.surfaceStatePresent)
        return {RhiRendererDrawEvidenceResult::Missing,
                RhiRendererDrawEvidenceReason::SemanticSurfaceStateMissing};
    if (colorTarget != nullptr && !renderer.colorTargetStatePresent)
        return {RhiRendererDrawEvidenceResult::Missing,
                RhiRendererDrawEvidenceReason::ColorTargetStateUnavailable};
    if (depthTarget != nullptr && !renderer.depthTargetStatePresent)
        return {RhiRendererDrawEvidenceResult::Missing,
                RhiRendererDrawEvidenceReason::DepthTargetStateUnavailable};
    if (colorTarget != nullptr && !colorTarget->normalizedStatePresent)
        return {RhiRendererDrawEvidenceResult::Missing,
                RhiRendererDrawEvidenceReason::ColorTargetStateMissing};
    if (depthTarget != nullptr && !depthTarget->normalizedStatePresent)
        return {RhiRendererDrawEvidenceResult::Missing,
                RhiRendererDrawEvidenceReason::DepthTargetStateMissing};
    if (state.surfaceState != renderer.surfaceState)
        return {RhiRendererDrawEvidenceResult::Mismatch,
                RhiRendererDrawEvidenceReason::SurfaceState};
    if (colorTarget != nullptr && colorTarget->normalizedState != renderer.colorTarget)
        return {RhiRendererDrawEvidenceResult::Mismatch,
                RhiRendererDrawEvidenceReason::ColorTargetState};
    if (depthTarget != nullptr && depthTarget->normalizedState != renderer.depthTarget)
        return {RhiRendererDrawEvidenceResult::Mismatch,
                RhiRendererDrawEvidenceReason::DepthTargetState};
    return {RhiRendererDrawEvidenceResult::Match, RhiRendererDrawEvidenceReason::None};
}

RhiRendererDrawEvidenceResult CompareRhiRendererDrawInput(const RhiSemanticDrawState &state,
                                                          const RhiRendererDrawInput &renderer)
{
    return InspectRhiRendererDrawInput(state, renderer).result;
}

RhiRendererFrameComparison CompareRhiRendererDraws(const RhiSemanticFrame &frame,
                                                   const RhiRendererFrameInput &renderer)
{
    RhiRendererFrameComparison result{.status = renderer.status,
                                      .sourceDraws = renderer.draws.size()};
    for (std::size_t index = 0; index < renderer.draws.size(); ++index)
    {
        const RhiRendererDrawInput &input = renderer.draws[index];
        if (input.sourceOrdinal != index)
        {
            ++result.mismatched;
            continue;
        }
        switch (input.outcome)
        {
        case draw::NativeDrawMaterializationOutcome::Materialized:
            ++result.materializedDraws;
            break;
        case draw::NativeDrawMaterializationOutcome::Refused:
            ++result.refusedDraws;
            break;
        case draw::NativeDrawMaterializationOutcome::Resolve:
            ++result.resolveDraws;
            break;
        }
    }

    for (const RhiSemanticEvent &event : frame.events)
    {
        if (std::holds_alternative<RhiObservedDraw>(event.payload))
            ++result.semanticDraws;
    }
    if (renderer.status != draw::NativeFrameMaterializationStatus::Complete)
    {
        result.missing += result.semanticDraws;
        return result;
    }

    std::map<std::uint32_t, std::vector<const RhiRendererDrawInput *>> packetExecutions;
    for (std::size_t index = 0; index < renderer.draws.size(); ++index)
    {
        const RhiRendererDrawInput &input = renderer.draws[index];
        if (input.outcome == draw::NativeDrawMaterializationOutcome::Resolve)
            continue;
        if (input.sourceOrdinal != index)
            continue;
        if (input.packetGuestAddress == 0)
        {
            ++result.unkeyedRendererDraws;
            continue;
        }
        packetExecutions[CanonicalPacketAddress(input.packetGuestAddress)].push_back(&input);
    }

    std::set<std::uint32_t> matchedPackets;
    for (const RhiSemanticEvent &event : frame.events)
    {
        const auto *observed = std::get_if<RhiObservedDraw>(&event.payload);
        if (observed == nullptr)
            continue;

        const std::uint32_t packetGuestAddress =
            CanonicalPacketAddress(observed->packet.packetGuestAddress);
        const auto executions = packetExecutions.find(packetGuestAddress);
        if (packetGuestAddress == 0 || executions == packetExecutions.end())
        {
            ++result.missing;
            if (result.firstMissingSemanticPacket == 0)
                result.firstMissingSemanticPacket = packetGuestAddress;
            continue;
        }
        if (!matchedPackets.insert(packetGuestAddress).second)
        {
            ++result.mismatched;
            continue;
        }

        bool missing = false;
        bool mismatch = false;
        RhiRendererDrawEvidenceReason mismatchReason = RhiRendererDrawEvidenceReason::None;
        for (const RhiRendererDrawInput *input : executions->second)
        {
            const RhiRendererDrawEvidence evidence =
                InspectRhiRendererDrawInput(observed->state, *input);
            switch (evidence.result)
            {
            case RhiRendererDrawEvidenceResult::Match:
                break;
            case RhiRendererDrawEvidenceResult::Missing:
                missing = true;
                break;
            case RhiRendererDrawEvidenceResult::Mismatch:
                mismatch = true;
                if (mismatchReason == RhiRendererDrawEvidenceReason::None)
                    mismatchReason = evidence.reason;
                break;
            }
        }
        if (mismatch)
        {
            ++result.mismatched;
            if (result.firstMismatchedSemanticPacket == 0)
            {
                result.firstMismatchedSemanticPacket = packetGuestAddress;
                result.firstMismatchReason = mismatchReason;
            }
        }
        else if (missing)
            ++result.missing;
        else
            ++result.matched;
    }
    for (const auto &[packetGuestAddress, executions] : packetExecutions)
    {
        (void)executions;
        if (!matchedPackets.contains(packetGuestAddress))
        {
            ++result.unmatchedRendererPackets;
            const RhiRendererDrawInput &first = *executions.front();
            const bool consistentOutcome =
                std::ranges::all_of(executions, [&first](const RhiRendererDrawInput *input)
                                    { return input->outcome == first.outcome; });
            if (!consistentOutcome)
                ++result.unmatchedRendererMixedOutcomePackets;
            else if (first.outcome == draw::NativeDrawMaterializationOutcome::Materialized)
                ++result.unmatchedRendererMaterializedPackets;
            else
                ++result.unmatchedRendererRefusedPackets;
            const bool consistentSource = std::ranges::all_of(
                executions,
                [&first](const RhiRendererDrawInput *input)
                {
                    return input->packetBufferBase == first.packetBufferBase &&
                           input->packetFromIndirectBuffer == first.packetFromIndirectBuffer;
                });
            if (!consistentSource)
                ++result.unmatchedRendererInconsistentSourcePackets;
            else if (first.packetFromIndirectBuffer)
                ++result.unmatchedRendererIndirectPackets;
            else
                ++result.unmatchedRendererRingPackets;
            if (result.firstUnmatchedRendererPacket == 0)
            {
                result.firstUnmatchedRendererPacket = packetGuestAddress;
                result.firstUnmatchedRendererBuffer = first.packetBufferBase;
                result.firstUnmatchedRendererFromIndirectBuffer = first.packetFromIndirectBuffer;
                result.firstUnmatchedRendererOutcome = first.outcome;
                result.firstUnmatchedRendererMixedOutcome = !consistentOutcome;
                result.firstUnmatchedRendererInconsistentSource = !consistentSource;
            }
        }
    }
    return result;
}

std::optional<RhiRendererFrameComparison> PublishRhiRendererFrameInput(std::uint64_t frameSequence,
                                                                       RhiRendererFrameInput frame)
{
    if (!RhiSemanticObservationEnabled())
        return std::nullopt;

    std::optional<RhiRendererFrameComparison> comparison;
    bool lateDuplicate = false;
    std::vector<PendingReport> staleReports;
    {
        std::lock_guard guard(g_join.mutex);
        const auto completed = g_join.completedFrames.find(frameSequence);
        if (completed != g_join.completedFrames.end() ||
            frameSequence <= g_join.retiredCompletedThrough)
        {
            comparison = InvalidateCompletedComparison(
                completed != g_join.completedFrames.end() ? &completed->second : nullptr);
            lateDuplicate = true;
        }
        else
        {
            const auto [it, inserted] = g_join.rendererFrames.emplace(
                frameSequence, RendererFrame{.input = std::move(frame)});
            if (!inserted)
            {
                it->second.valid = false;
                it->second.input = {
                    .status = draw::NativeFrameMaterializationStatus::RendererUnavailable};
                lucent::error("rhi", "renderer materialization was submitted twice for frame {}",
                              frameSequence);
            }
            comparison = TakeComparisonLocked(frameSequence);
            staleReports = RetireStaleJoinsLocked(frameSequence);
        }
    }
    if (lateDuplicate)
        ReportLateDuplicate(frameSequence);
    else if (comparison.has_value())
        ReportComparison(frameSequence, *comparison);
    ReportAll(staleReports);
    return comparison;
}

std::optional<RhiRendererFrameComparison>
ObserveRhiSemanticFrameSealed(const RhiSemanticFrame &frame)
{
    std::optional<RhiRendererFrameComparison> comparison;
    bool lateDuplicate = false;
    std::vector<PendingReport> staleReports;
    {
        std::lock_guard guard(g_join.mutex);
        const auto completed = g_join.completedFrames.find(frame.frameSequence);
        if (completed != g_join.completedFrames.end() ||
            frame.frameSequence <= g_join.retiredCompletedThrough)
        {
            comparison = InvalidateCompletedComparison(
                completed != g_join.completedFrames.end() ? &completed->second : nullptr);
            lateDuplicate = true;
        }
        else
        {
            const auto [it, inserted] =
                g_join.semanticFrames.emplace(frame.frameSequence, SemanticFrame{.input = frame});
            if (!inserted)
            {
                lucent::error("rhi",
                              "semantic frame {} was sealed twice before renderer completion",
                              frame.frameSequence);
                it->second.valid = false;
            }
            comparison = TakeComparisonLocked(frame.frameSequence);
            staleReports = RetireStaleJoinsLocked(frame.frameSequence);
        }
    }
    if (lateDuplicate)
        ReportLateDuplicate(frame.frameSequence);
    else if (comparison.has_value())
        ReportComparison(frame.frameSequence, *comparison);
    ReportAll(staleReports);
    return comparison;
}

void ObserveRhiRendererMaterialization(std::uint64_t frameSequence,
                                       const draw::NativeFrameMaterialization &materialization)
{
    if (!RhiSemanticObservationEnabled())
        return;

    RhiRendererFrameInput frame{.status = materialization.status};
    frame.draws.reserve(materialization.draws.size());
    for (const draw::NativeDrawMaterialization &draw : materialization.draws)
    {
        frame.draws.push_back(
            {.sourceOrdinal = draw.sourceOrdinal,
             .packetGuestAddress = draw.packetGuestAddress,
             .packetBufferBase = draw.packetBufferBase,
             .packetFromIndirectBuffer = draw.packetFromIndirectBuffer,
             .outcome = draw.outcome,
             .primitiveType = draw.input.primitiveType,
             .elementCount = draw.input.indexCount,
             .indexed = draw.input.indexed,
             .indexIs32 = draw.input.indexIs32,
             .indexEndian = draw.input.indexEndian,
             .indexGuestBase = draw.input.indexGuestBase,
             .targetStatePresent =
                 draw.outcome == draw::NativeDrawMaterializationOutcome::Materialized,
             .colorTargetStatePresent =
                 draw.outcome == draw::NativeDrawMaterializationOutcome::Materialized,
             .depthTargetStatePresent =
                 draw.outcome == draw::NativeDrawMaterializationOutcome::Materialized,
             .colorTarget = {.base = draw.input.surfaceBase,
                             .format = draw.input.colorFormat,
                             .colorExponentBias = draw.input.colorExpBias},
             .depthTarget = {.base = draw.input.depthBase,
                             .format = draw.input.depthIsFloat24 ? 1u : 0u},
             .surfaceState = DecodeRhiSurfaceState(draw.input.surfaceInfo)});
    }
    (void)PublishRhiRendererFrameInput(frameSequence, std::move(frame));
}

void ObserveRhiRendererFrameDropped(std::uint64_t frameSequence)
{
    if (!RhiSemanticObservationEnabled())
        return;
    (void)PublishRhiRendererFrameInput(frameSequence,
                                       {.status = draw::NativeFrameMaterializationStatus::Dropped});
}

void SetRhiPacketIdentity(FrameDrawItem &draw, std::uint32_t sourceBase, std::uint32_t ringBase,
                          std::uint32_t sourceIndex)
{
    draw.packetGuestAddress = (sourceBase != 0 ? sourceBase : ringBase) + sourceIndex * 4;
    draw.packetBufferBase = sourceBase != 0 ? sourceBase : ringBase;
    draw.packetFromIndirectBuffer = sourceBase != 0;
}

RhiRendererMaterializationGuard::RhiRendererMaterializationGuard(std::uint64_t frameSequence)
    : frameSequence_(frameSequence), pending_(RhiSemanticObservationEnabled())
{
}

RhiRendererMaterializationGuard::~RhiRendererMaterializationGuard()
{
    if (pending_)
        ObserveRhiRendererFrameDropped(frameSequence_);
}

void RhiRendererMaterializationGuard::Attach(FrameDrawInputs &inputs)
{
    if (!pending_)
        return;
    inputs.materializationCallback = ObserveRhiRendererMaterialization;
    pending_ = false;
}

} // namespace gears
