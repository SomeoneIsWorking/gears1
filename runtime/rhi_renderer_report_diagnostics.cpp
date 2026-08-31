#include "rhi_renderer_report_diagnostics.h"

#include "rhi_renderer_input.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>

#include <lucent/log.h>

namespace gears
{

const char *RhiRendererEvidenceReasonName(RhiRendererDrawEvidenceReason reason)
{
    switch (reason)
    {
    case RhiRendererDrawEvidenceReason::None:
        return "none";
    case RhiRendererDrawEvidenceReason::RendererRefused:
        return "renderer-refused";
    case RhiRendererDrawEvidenceReason::RendererSourceOrdinal:
        return "renderer-source-ordinal";
    case RhiRendererDrawEvidenceReason::DuplicateSemanticPacket:
        return "duplicate-semantic-packet";
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
    case RhiRendererDrawEvidenceReason::SemanticVertexShaderMissing:
        return "semantic-vertex-shader-missing";
    case RhiRendererDrawEvidenceReason::SemanticPixelShaderMissing:
        return "semantic-pixel-shader-missing";
    case RhiRendererDrawEvidenceReason::RendererVertexShaderMissing:
        return "renderer-vertex-shader-missing";
    case RhiRendererDrawEvidenceReason::RendererPixelShaderMissing:
        return "renderer-pixel-shader-missing";
    case RhiRendererDrawEvidenceReason::SemanticVertexShaderModulesMissing:
        return "semantic-vertex-shader-modules-missing";
    case RhiRendererDrawEvidenceReason::SemanticPixelShaderModulesMissing:
        return "semantic-pixel-shader-modules-missing";
    case RhiRendererDrawEvidenceReason::SemanticVertexShaderModulesAmbiguous:
        return "semantic-vertex-shader-modules-ambiguous";
    case RhiRendererDrawEvidenceReason::SemanticPixelShaderModulesAmbiguous:
        return "semantic-pixel-shader-modules-ambiguous";
    case RhiRendererDrawEvidenceReason::VertexShaderModule:
        return "vertex-shader-module";
    case RhiRendererDrawEvidenceReason::PixelShaderModule:
        return "pixel-shader-module";
    case RhiRendererDrawEvidenceReason::DuplicateTextureSlot:
        return "duplicate-texture-slot";
    case RhiRendererDrawEvidenceReason::UnsupportedTextureSlot:
        return "unsupported-texture-slot";
    case RhiRendererDrawEvidenceReason::RendererTextureStateMissing:
        return "renderer-texture-state-missing";
    case RhiRendererDrawEvidenceReason::SemanticTextureStateMissing:
        return "semantic-texture-state-missing";
    case RhiRendererDrawEvidenceReason::TextureState:
        return "texture-state";
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
    case RhiRendererDrawEvidenceReason::RendererViewportStateMissing:
        return "renderer-viewport-state-missing";
    case RhiRendererDrawEvidenceReason::SemanticViewportStateMissing:
        return "semantic-viewport-state-missing";
    case RhiRendererDrawEvidenceReason::ViewportState:
        return "viewport-state";
    case RhiRendererDrawEvidenceReason::Count:
        return "count";
    }
    return "unknown";
}

namespace
{

struct RhiRendererEvidenceCensus
{
    std::array<std::uint64_t, 4> unkeyedSemanticPacketKinds{};
    std::array<std::uint64_t, kRhiSemanticBindingOriginCount> pixelShaderBindingsByOrigin{};
    std::array<std::uint64_t, kRhiSemanticBindingOriginCount> pixelShaderClearsByOrigin{};
    std::array<std::uint64_t, kRhiSemanticBindingOriginCount>
        missingPixelShaderLastBindingsByOrigin{};
    std::array<std::uint64_t, kRhiSemanticBindingOriginCount>
        missingPixelShaderLastClearsByOrigin{};
    std::array<std::uint64_t, static_cast<std::size_t>(RhiRendererDrawEvidenceReason::Count)>
        missingEvidenceReasons{};
};

RhiRendererEvidenceCensus g_census;
std::mutex g_censusMutex;

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

[[nodiscard]] const char *BindingOriginName(RhiSemanticBindingOrigin origin)
{
    switch (origin)
    {
    case RhiSemanticBindingOrigin::Unknown:
        return "unknown";
    case RhiSemanticBindingOrigin::Setter:
        return "setter";
    case RhiSemanticBindingOrigin::Flush:
        return "flush";
    }
    return "unknown";
}

} // namespace

std::string DescribeRhiRendererUnmatchedPacket(const RhiRendererFrameComparison &comparison)
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

void ReportRhiRendererEvidenceCensus(const RhiRendererFrameComparison &comparison)
{
    std::lock_guard guard(g_censusMutex);
    for (std::size_t index = 0; index < comparison.unkeyedSemanticPacketKinds.size(); ++index)
        g_census.unkeyedSemanticPacketKinds[index] += comparison.unkeyedSemanticPacketKinds[index];
    for (std::size_t index = 0; index < comparison.pixelShaderBindingsByOrigin.size(); ++index)
    {
        g_census.pixelShaderBindingsByOrigin[index] +=
            comparison.pixelShaderBindingsByOrigin[index];
        g_census.pixelShaderClearsByOrigin[index] += comparison.pixelShaderClearsByOrigin[index];
        g_census.missingPixelShaderLastBindingsByOrigin[index] +=
            comparison.missingPixelShaderLastBindingsByOrigin[index];
        g_census.missingPixelShaderLastClearsByOrigin[index] +=
            comparison.missingPixelShaderLastClearsByOrigin[index];
    }
    for (std::size_t index = 0; index < comparison.missingEvidenceReasons.size(); ++index)
        g_census.missingEvidenceReasons[index] += comparison.missingEvidenceReasons[index];

    if (comparison.missing == 0 &&
        comparison.unkeyedSemanticPacketKinds == std::array<std::uint64_t, 4>{})
        return;

    lucent::info("rhi",
                 "  terminal semantic evidence census: unkeyed packet transient-vertices {},"
                 " transient-vertices-and-indices {}, bound-vertices {}, bound-indices {}",
                 g_census.unkeyedSemanticPacketKinds[0], g_census.unkeyedSemanticPacketKinds[1],
                 g_census.unkeyedSemanticPacketKinds[2], g_census.unkeyedSemanticPacketKinds[3]);
    for (std::size_t index = 0; index < g_census.pixelShaderBindingsByOrigin.size(); ++index)
    {
        if (g_census.pixelShaderBindingsByOrigin[index] != 0)
            lucent::info("rhi", "  pixel shader {} binding(s) x{}, effective clear(s) x{}",
                         BindingOriginName(static_cast<RhiSemanticBindingOrigin>(index)),
                         g_census.pixelShaderBindingsByOrigin[index],
                         g_census.pixelShaderClearsByOrigin[index]);
        if (g_census.missingPixelShaderLastBindingsByOrigin[index] != 0)
            lucent::info("rhi",
                         "  missing pixel shader draw(s) with last {} binding x{},"
                         " last clear x{}",
                         BindingOriginName(static_cast<RhiSemanticBindingOrigin>(index)),
                         g_census.missingPixelShaderLastBindingsByOrigin[index],
                         g_census.missingPixelShaderLastClearsByOrigin[index]);
    }
    for (std::size_t index = 0; index < g_census.missingEvidenceReasons.size(); ++index)
    {
        if (g_census.missingEvidenceReasons[index] != 0)
            lucent::info(
                "rhi", "  missing renderer evidence {} x{}",
                RhiRendererEvidenceReasonName(static_cast<RhiRendererDrawEvidenceReason>(index)),
                g_census.missingEvidenceReasons[index]);
    }
}

} // namespace gears
