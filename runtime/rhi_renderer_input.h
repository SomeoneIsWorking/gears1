#pragma once

#include "gpu_draw_native_input.h"
#include "rhi_semantic_stream.h"
#include "rhi_target_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gears
{

struct RhiSemanticDrawState;
struct RhiSemanticFrame;
struct FrameDrawInputs;
struct FrameDrawItem;

// A terminal projection of one source draw after the compatibility renderer
// either materialized NativeDrawInput or explicitly refused it. Source ordinals
// preserve gaps; a refused draw cannot shift every later comparison.
struct RhiRendererDrawInput
{
    std::size_t sourceOrdinal = 0;
    std::uint32_t packetGuestAddress = 0;
    std::uint32_t packetBufferBase = 0;
    bool packetFromIndirectBuffer = false;
    draw::NativeDrawMaterializationOutcome outcome =
        draw::NativeDrawMaterializationOutcome::Refused;
    std::uint32_t primitiveType = 0;
    std::uint32_t elementCount = 0;
    bool indexed = false;
    bool indexIs32 = false;
    std::uint32_t indexEndian = 0;
    std::uint32_t indexGuestBase = 0;
    std::uint64_t vertexShaderHash = 0;
    std::uint64_t pixelShaderHash = 0;
    bool textureFetchStatePresent = false;
    draw::NativeTextureFetchFile textureFetches{};
    // These flags mean the terminal renderer supplied comparable normalized
    // register state. Semantic bindings remain the authority for whether a
    // title resource is active; backend attachment allocation is not presence.
    bool targetStatePresent = false;
    bool colorTargetStatePresent = false;
    bool depthTargetStatePresent = false;
    RhiRenderTargetDescriptorState colorTarget;
    RhiRenderTargetDescriptorState depthTarget;
    RhiSurfaceState surfaceState;
};

struct RhiRendererFrameInput
{
    draw::NativeFrameMaterializationStatus status =
        draw::NativeFrameMaterializationStatus::Complete;
    std::vector<RhiRendererDrawInput> draws;
};

enum class RhiRendererDrawEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

enum class RhiRendererDrawEvidenceReason : std::uint8_t
{
    None,
    RendererRefused,
    RendererSourceOrdinal,
    DuplicateSemanticPacket,
    DrawShape,
    IndexBufferViewMissing,
    IndexWidth,
    IndexAddress,
    IndexEndian,
    SemanticVertexShaderMissing,
    SemanticPixelShaderMissing,
    RendererVertexShaderMissing,
    RendererPixelShaderMissing,
    SemanticVertexShaderModulesMissing,
    SemanticPixelShaderModulesMissing,
    SemanticVertexShaderModulesAmbiguous,
    SemanticPixelShaderModulesAmbiguous,
    VertexShaderModule,
    PixelShaderModule,
    DuplicateTextureSlot,
    UnsupportedTextureSlot,
    RendererTextureStateMissing,
    SemanticTextureStateMissing,
    TextureState,
    DuplicateColorTarget,
    DuplicateDepthTarget,
    UnsupportedColorTargetSlot,
    RendererTargetStateMissing,
    SemanticSurfaceStateMissing,
    ColorTargetStateUnavailable,
    DepthTargetStateUnavailable,
    ColorTargetStateMissing,
    DepthTargetStateMissing,
    SurfaceState,
    ColorTargetState,
    DepthTargetState,
    Count,
};

struct RhiRendererDrawEvidence
{
    RhiRendererDrawEvidenceResult result = RhiRendererDrawEvidenceResult::Missing;
    RhiRendererDrawEvidenceReason reason = RhiRendererDrawEvidenceReason::None;
    bool textureMismatchPresent = false;
    std::uint32_t textureSlot = 0;
    std::uint32_t textureDword = 0;
    std::uint32_t semanticTextureValue = 0;
    std::uint32_t rendererTextureValue = 0;
    bool vertexShaderModuleMatched = false;
    bool pixelShaderModuleMatched = false;
    bool shaderMismatchPresent = false;
    bool vertexShaderMismatch = false;
    std::uint32_t semanticShaderObject = 0;
    std::uint64_t rendererShaderHash = 0;
    std::array<std::uint64_t, 2> semanticShaderModuleHashes{};
    std::uint32_t semanticShaderModuleCount = 0;
};

struct RhiRendererFrameComparison
{
    draw::NativeFrameMaterializationStatus status =
        draw::NativeFrameMaterializationStatus::Complete;
    std::uint64_t semanticDraws = 0;
    std::uint64_t sourceDraws = 0;
    std::uint64_t materializedDraws = 0;
    std::uint64_t refusedDraws = 0;
    std::uint64_t resolveDraws = 0;
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t mismatched = 0;
    std::array<std::uint64_t, 4> unkeyedSemanticPacketKinds{};
    std::array<std::uint64_t, kRhiSemanticBindingOriginCount> pixelShaderBindingsByOrigin{};
    std::array<std::uint64_t, kRhiSemanticBindingOriginCount> pixelShaderClearsByOrigin{};
    std::array<std::uint64_t, kRhiSemanticBindingOriginCount>
        missingPixelShaderLastBindingsByOrigin{};
    std::array<std::uint64_t, kRhiSemanticBindingOriginCount>
        missingPixelShaderLastClearsByOrigin{};
    std::array<std::uint64_t, static_cast<std::size_t>(RhiRendererDrawEvidenceReason::Count)>
        missingEvidenceReasons{};
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
    std::uint32_t firstMissingSemanticPacket = 0;
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
    std::uint32_t firstUnmatchedRendererPacket = 0;
    std::uint32_t firstUnmatchedRendererBuffer = 0;
    bool firstUnmatchedRendererFromIndirectBuffer = false;
    draw::NativeDrawMaterializationOutcome firstUnmatchedRendererOutcome =
        draw::NativeDrawMaterializationOutcome::Refused;
    bool firstUnmatchedRendererMixedOutcome = false;
    bool firstUnmatchedRendererInconsistentSource = false;
    bool duplicate = false;
};

[[nodiscard]] RhiRendererDrawEvidenceResult
CompareRhiRendererDrawInput(const RhiSemanticDrawState &state,
                            const RhiRendererDrawInput &renderer);
[[nodiscard]] RhiRendererDrawEvidence
InspectRhiRendererDrawInput(const RhiSemanticDrawState &state,
                            const RhiRendererDrawInput &renderer);
[[nodiscard]] RhiRendererFrameComparison
CompareRhiRendererDraws(const RhiSemanticFrame &frame, const RhiRendererFrameInput &renderer);
void ObserveRhiRendererMaterialization(std::uint64_t frameSequence,
                                       const draw::NativeFrameMaterialization &materialization);
void ObserveRhiRendererFrameDropped(std::uint64_t frameSequence);
void SetRhiPacketIdentity(FrameDrawItem &draw, std::uint32_t sourceBase, std::uint32_t ringBase,
                          std::uint32_t sourceIndex);
[[nodiscard]] std::optional<RhiRendererFrameComparison>
PublishRhiRendererFrameInput(std::uint64_t frameSequence, RhiRendererFrameInput frame);

// Completes the asynchronous join when the title-semantic side arrives after
// or before renderer materialization. This is an internal production boundary,
// separate from semantic event capture and state tracking.
[[nodiscard]] std::optional<RhiRendererFrameComparison>
ObserveRhiSemanticFrameSealed(const RhiSemanticFrame &frame);

// Makes every command-processor frame attempt terminal: early exits publish a
// dropped result, while Attach transfers publication to the renderer callback.
class RhiRendererMaterializationGuard
{
  public:
    explicit RhiRendererMaterializationGuard(std::uint64_t frameSequence);
    ~RhiRendererMaterializationGuard();

    RhiRendererMaterializationGuard(const RhiRendererMaterializationGuard &) = delete;
    RhiRendererMaterializationGuard &operator=(const RhiRendererMaterializationGuard &) = delete;

    void Attach(FrameDrawInputs &inputs);

  private:
    std::uint64_t frameSequence_ = 0;
    bool pending_ = false;
};

} // namespace gears
