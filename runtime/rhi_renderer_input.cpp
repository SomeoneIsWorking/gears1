#include "rhi_renderer_input.h"

#include "guest_address.h"
#include "gpu_draw.h"
#include "rhi_renderer_report_diagnostics.h"
#include "rhi_semantic_stream.h"

#include <algorithm>
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
    std::uint64_t vertexShaderModuleMatches = 0;
    std::uint64_t pixelShaderModuleMatches = 0;
    std::uint64_t unmatchedRendererPackets = 0;
    std::uint64_t unmatchedRendererMaterializedPackets = 0;
    std::uint64_t unmatchedRendererRefusedPackets = 0;
    std::uint64_t unmatchedRendererMixedOutcomePackets = 0;
    std::uint64_t unmatchedRendererIndirectPackets = 0;
    std::uint64_t unmatchedRendererRingPackets = 0;
    std::uint64_t unmatchedRendererInconsistentSourcePackets = 0;
    std::uint64_t unkeyedRendererDraws = 0;
    std::uint64_t duplicateFrames = 0;
    bool firstMissingPresent = false;
    std::uint32_t firstMissingSemanticPacket = 0;
    bool firstMismatchPresent = false;
    std::uint32_t firstMismatchedSemanticPacket = 0;
    RhiRendererDrawEvidenceReason firstMismatchReason = RhiRendererDrawEvidenceReason::None;
    bool firstTextureMismatchPresent = false;
    std::uint32_t firstTextureMismatchSlot = 0;
    std::uint32_t firstTextureMismatchDword = 0;
    std::uint32_t firstSemanticTextureValue = 0;
    std::uint32_t firstRendererTextureValue = 0;
    bool firstShaderMismatchPresent = false;
    bool firstVertexShaderMismatch = false;
    std::uint32_t firstSemanticShaderObject = 0;
    std::uint64_t firstRendererShaderHash = 0;
    std::array<std::uint64_t, 2> firstSemanticShaderModuleHashes{};
    std::uint32_t firstSemanticShaderModuleCount = 0;
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

[[nodiscard]] std::uint32_t CanonicalPacketAddress(std::uint32_t guestAddress)
{
    return guestAddress & kGuestPhysicalAddressMask & ~std::uint32_t{3};
}

[[nodiscard]] RhiRendererDrawEvidence
ShaderMismatchEvidence(bool vertex, const RhiSemanticBinding &binding, std::uint64_t rendererHash)
{
    RhiRendererDrawEvidence evidence{
        .result = RhiRendererDrawEvidenceResult::Mismatch,
        .reason = vertex ? RhiRendererDrawEvidenceReason::VertexShaderModule
                         : RhiRendererDrawEvidenceReason::PixelShaderModule,
        .shaderMismatchPresent = true,
        .vertexShaderMismatch = vertex,
        .semanticShaderObject = binding.object,
        .rendererShaderHash = rendererHash,
        .semanticShaderModuleCount = static_cast<std::uint32_t>(binding.shaderModules.size()),
    };
    const std::size_t count =
        std::min(evidence.semanticShaderModuleHashes.size(), binding.shaderModules.size());
    for (std::size_t index = 0; index < count; ++index)
        evidence.semanticShaderModuleHashes[index] = binding.shaderModules[index].hash;
    return evidence;
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
    g_reportTotals.vertexShaderModuleMatches += comparison.vertexShaderModuleMatches;
    g_reportTotals.pixelShaderModuleMatches += comparison.pixelShaderModuleMatches;
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
    if (!g_reportTotals.firstMissingPresent && comparison.missing != 0)
    {
        g_reportTotals.firstMissingPresent = true;
        g_reportTotals.firstMissingSemanticPacket = comparison.firstMissingSemanticPacket;
    }
    if (!g_reportTotals.firstMismatchPresent && comparison.mismatched != 0)
    {
        g_reportTotals.firstMismatchPresent = true;
        g_reportTotals.firstMismatchedSemanticPacket = comparison.firstMismatchedSemanticPacket;
        g_reportTotals.firstMismatchReason = comparison.firstMismatchReason;
        g_reportTotals.firstTextureMismatchPresent = comparison.firstTextureMismatchPresent;
        g_reportTotals.firstTextureMismatchSlot = comparison.firstTextureMismatchSlot;
        g_reportTotals.firstTextureMismatchDword = comparison.firstTextureMismatchDword;
        g_reportTotals.firstSemanticTextureValue = comparison.firstSemanticTextureValue;
        g_reportTotals.firstRendererTextureValue = comparison.firstRendererTextureValue;
        g_reportTotals.firstShaderMismatchPresent = comparison.firstShaderMismatchPresent;
        g_reportTotals.firstVertexShaderMismatch = comparison.firstVertexShaderMismatch;
        g_reportTotals.firstSemanticShaderObject = comparison.firstSemanticShaderObject;
        g_reportTotals.firstRendererShaderHash = comparison.firstRendererShaderHash;
        g_reportTotals.firstSemanticShaderModuleHashes = comparison.firstSemanticShaderModuleHashes;
        g_reportTotals.firstSemanticShaderModuleCount = comparison.firstSemanticShaderModuleCount;
    }

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
            " semantic match(es), {} missing, {} mismatch(es); exact shader module matches"
            " vertex {}, pixel {}; {} unmatched renderer"
            " packet(s) ({} materialized, {} refused, {} mixed outcome; {} indirect, {} ring,"
            " {} source conflict), {} unkeyed"
            " renderer draw(s), {} duplicate frame(s); current frame"
            " {}{}; first missing semantic packet {:#x}, first mismatched semantic packet"
            " {:#x} ({}; texture detail {} slot {} dword {} semantic {:#x} renderer {:#x}),"
            " shader detail {} {} object {:#x} renderer {:#018x} modules {}"
            " [{:#018x}, {:#018x}],"
            " first"
            " unmatched renderer packet {}",
            frameSequence, g_reportTotals.frames, g_reportTotals.unavailableFrames,
            g_reportTotals.droppedFrames, g_reportTotals.sourceDraws,
            g_reportTotals.materializedDraws, g_reportTotals.refusedDraws,
            g_reportTotals.resolveDraws, g_reportTotals.matched, g_reportTotals.missing,
            g_reportTotals.mismatched, g_reportTotals.vertexShaderModuleMatches,
            g_reportTotals.pixelShaderModuleMatches, g_reportTotals.unmatchedRendererPackets,
            g_reportTotals.unmatchedRendererMaterializedPackets,
            g_reportTotals.unmatchedRendererRefusedPackets,
            g_reportTotals.unmatchedRendererMixedOutcomePackets,
            g_reportTotals.unmatchedRendererIndirectPackets,
            g_reportTotals.unmatchedRendererRingPackets,
            g_reportTotals.unmatchedRendererInconsistentSourcePackets,
            g_reportTotals.unkeyedRendererDraws, g_reportTotals.duplicateFrames,
            FrameStatusName(comparison.status), comparison.duplicate ? ", duplicate" : "",
            g_reportTotals.firstMissingSemanticPacket, g_reportTotals.firstMismatchedSemanticPacket,
            RhiRendererEvidenceReasonName(g_reportTotals.firstMismatchReason),
            g_reportTotals.firstTextureMismatchPresent ? "present" : "absent",
            g_reportTotals.firstTextureMismatchSlot, g_reportTotals.firstTextureMismatchDword,
            g_reportTotals.firstSemanticTextureValue, g_reportTotals.firstRendererTextureValue,
            g_reportTotals.firstShaderMismatchPresent ? "present" : "absent",
            g_reportTotals.firstVertexShaderMismatch ? "vertex" : "pixel",
            g_reportTotals.firstSemanticShaderObject, g_reportTotals.firstRendererShaderHash,
            g_reportTotals.firstSemanticShaderModuleCount,
            g_reportTotals.firstSemanticShaderModuleHashes[0],
            g_reportTotals.firstSemanticShaderModuleHashes[1],
            DescribeRhiRendererUnmatchedPacket(comparison));
        ReportRhiRendererEvidenceCensus(comparison);
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

    RhiRendererDrawEvidence evidence{.result = RhiRendererDrawEvidenceResult::Match};
    if (renderer.vertexShaderHash != 0 && !state.vertexShader.has_value())
        return {RhiRendererDrawEvidenceResult::Missing,
                RhiRendererDrawEvidenceReason::SemanticVertexShaderMissing};
    if (state.vertexShader.has_value())
    {
        if (renderer.vertexShaderHash == 0)
            return {RhiRendererDrawEvidenceResult::Missing,
                    RhiRendererDrawEvidenceReason::RendererVertexShaderMissing};
        if (state.vertexShader->shaderModules.empty())
            return {RhiRendererDrawEvidenceResult::Missing,
                    RhiRendererDrawEvidenceReason::SemanticVertexShaderModulesMissing};
        if (state.vertexShader->shaderModules.size() != 1)
            return {RhiRendererDrawEvidenceResult::Missing,
                    RhiRendererDrawEvidenceReason::SemanticVertexShaderModulesAmbiguous};
        if (state.vertexShader->shaderModules.front().hash != renderer.vertexShaderHash)
            return ShaderMismatchEvidence(true, *state.vertexShader, renderer.vertexShaderHash);
        evidence.vertexShaderModuleMatched = true;
    }

    if (renderer.pixelShaderHash != 0 && !state.pixelShader.has_value())
        return {RhiRendererDrawEvidenceResult::Missing,
                RhiRendererDrawEvidenceReason::SemanticPixelShaderMissing};
    if (state.pixelShader.has_value())
    {
        if (renderer.pixelShaderHash == 0)
            return {RhiRendererDrawEvidenceResult::Missing,
                    RhiRendererDrawEvidenceReason::RendererPixelShaderMissing};
        if (state.pixelShader->shaderModules.empty())
            return {RhiRendererDrawEvidenceResult::Missing,
                    RhiRendererDrawEvidenceReason::SemanticPixelShaderModulesMissing};
        if (state.pixelShader->shaderModules.size() != 1)
            return {RhiRendererDrawEvidenceResult::Missing,
                    RhiRendererDrawEvidenceReason::SemanticPixelShaderModulesAmbiguous};
        if (state.pixelShader->shaderModules.front().hash != renderer.pixelShaderHash)
            return ShaderMismatchEvidence(false, *state.pixelShader, renderer.pixelShaderHash);
        evidence.pixelShaderModuleMatched = true;
    }

    std::array<bool, draw::kNativeTextureFetchSlots> textureSlots{};
    for (const RhiSemanticBinding &texture : state.textures)
    {
        if (texture.slot >= draw::kNativeTextureFetchSlots)
            return {RhiRendererDrawEvidenceResult::Missing,
                    RhiRendererDrawEvidenceReason::UnsupportedTextureSlot};
        if (textureSlots[texture.slot])
            return {RhiRendererDrawEvidenceResult::Mismatch,
                    RhiRendererDrawEvidenceReason::DuplicateTextureSlot};
        textureSlots[texture.slot] = true;
        if (texture.descriptorDwords != draw::kNativeTextureFetchDwords)
            return {RhiRendererDrawEvidenceResult::Missing,
                    RhiRendererDrawEvidenceReason::SemanticTextureStateMissing};
        if (!renderer.textureFetchStatePresent)
            return {RhiRendererDrawEvidenceResult::Missing,
                    RhiRendererDrawEvidenceReason::RendererTextureStateMissing};
        for (std::size_t dword = 0; dword < draw::kNativeTextureFetchDwords; ++dword)
        {
            if (texture.descriptor[dword] != renderer.textureFetches[texture.slot][dword])
            {
                return {
                    .result = RhiRendererDrawEvidenceResult::Mismatch,
                    .reason = RhiRendererDrawEvidenceReason::TextureState,
                    .textureMismatchPresent = true,
                    .textureSlot = texture.slot,
                    .textureDword = static_cast<std::uint32_t>(dword),
                    .semanticTextureValue = texture.descriptor[dword],
                    .rendererTextureValue = renderer.textureFetches[texture.slot][dword],
                };
            }
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
        return evidence;
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
    return evidence;
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
            if (result.firstMismatchReason == RhiRendererDrawEvidenceReason::None)
            {
                result.firstMismatchedSemanticPacket =
                    CanonicalPacketAddress(input.packetGuestAddress);
                result.firstMismatchReason = RhiRendererDrawEvidenceReason::RendererSourceOrdinal;
            }
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
        if (const auto *binding = std::get_if<RhiObservedBinding>(&event.payload);
            binding != nullptr && binding->binding.kind == RhiSemanticBindingKind::PixelShader)
        {
            const std::size_t origin = static_cast<std::size_t>(binding->binding.origin);
            ++result.pixelShaderBindingsByOrigin[origin];
            const std::uint32_t effectiveObject =
                binding->state.present ? binding->state.observedObject : binding->binding.object;
            if (effectiveObject == 0)
                ++result.pixelShaderClearsByOrigin[origin];
        }
        else if (const auto *observed = std::get_if<RhiObservedDraw>(&event.payload);
                 observed != nullptr)
        {
            ++result.semanticDraws;
            if (CanonicalPacketAddress(observed->packet.packetGuestAddress) == 0)
                ++result.unkeyedSemanticPacketKinds[static_cast<std::size_t>(
                    observed->state.draw.kind)];
        }
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
            if (result.firstMismatchReason == RhiRendererDrawEvidenceReason::None)
            {
                result.firstMismatchedSemanticPacket = packetGuestAddress;
                result.firstMismatchReason = RhiRendererDrawEvidenceReason::DuplicateSemanticPacket;
            }
            continue;
        }

        bool missing = false;
        bool mismatch = false;
        RhiRendererDrawEvidence firstMissingEvidence;
        RhiRendererDrawEvidence firstMismatchEvidence;
        for (const RhiRendererDrawInput *input : executions->second)
        {
            const RhiRendererDrawEvidence evidence =
                InspectRhiRendererDrawInput(observed->state, *input);
            switch (evidence.result)
            {
            case RhiRendererDrawEvidenceResult::Match:
                if (evidence.vertexShaderModuleMatched)
                    ++result.vertexShaderModuleMatches;
                if (evidence.pixelShaderModuleMatched)
                    ++result.pixelShaderModuleMatches;
                break;
            case RhiRendererDrawEvidenceResult::Missing:
                missing = true;
                if (firstMissingEvidence.reason == RhiRendererDrawEvidenceReason::None)
                    firstMissingEvidence = evidence;
                break;
            case RhiRendererDrawEvidenceResult::Mismatch:
                mismatch = true;
                if (firstMismatchEvidence.reason == RhiRendererDrawEvidenceReason::None)
                    firstMismatchEvidence = evidence;
                break;
            }
        }
        if (mismatch)
        {
            ++result.mismatched;
            if (result.firstMismatchReason == RhiRendererDrawEvidenceReason::None)
            {
                result.firstMismatchedSemanticPacket = packetGuestAddress;
                result.firstMismatchReason = firstMismatchEvidence.reason;
                if (firstMismatchEvidence.textureMismatchPresent)
                {
                    result.firstTextureMismatchPresent = true;
                    result.firstTextureMismatchSlot = firstMismatchEvidence.textureSlot;
                    result.firstTextureMismatchDword = firstMismatchEvidence.textureDword;
                    result.firstSemanticTextureValue = firstMismatchEvidence.semanticTextureValue;
                    result.firstRendererTextureValue = firstMismatchEvidence.rendererTextureValue;
                }
                if (firstMismatchEvidence.shaderMismatchPresent)
                {
                    result.firstShaderMismatchPresent = true;
                    result.firstVertexShaderMismatch = firstMismatchEvidence.vertexShaderMismatch;
                    result.firstSemanticShaderObject = firstMismatchEvidence.semanticShaderObject;
                    result.firstRendererShaderHash = firstMismatchEvidence.rendererShaderHash;
                    result.firstSemanticShaderModuleHashes =
                        firstMismatchEvidence.semanticShaderModuleHashes;
                    result.firstSemanticShaderModuleCount =
                        firstMismatchEvidence.semanticShaderModuleCount;
                }
            }
        }
        else if (missing)
        {
            ++result.missing;
            ++result.missingEvidenceReasons[static_cast<std::size_t>(firstMissingEvidence.reason)];
            if (firstMissingEvidence.reason ==
                    RhiRendererDrawEvidenceReason::SemanticPixelShaderMissing &&
                observed->state.lastPixelShaderBinding.has_value())
            {
                const RhiSemanticBinding &last = *observed->state.lastPixelShaderBinding;
                const std::size_t origin = static_cast<std::size_t>(last.origin);
                ++result.missingPixelShaderLastBindingsByOrigin[origin];
                if (last.object == 0)
                    ++result.missingPixelShaderLastClearsByOrigin[origin];
            }
        }
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
             .vertexShaderHash = draw.input.vertexShaderHash,
             .pixelShaderHash = draw.input.pixelShaderHash,
             .textureFetchStatePresent =
                 draw.outcome == draw::NativeDrawMaterializationOutcome::Materialized,
             .textureFetches = draw.input.textureFetches,
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
