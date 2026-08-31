#include "rhi_renderer_input.h"
#include "rhi_semantic_stream.h"
#include "gpu_draw.h"

#include <cassert>
#include <cstdlib>
#include <optional>
#include <variant>
#include <vector>

#include <lucent/config.h>

int main()
{
    using gears::CompareRhiBindingState;
    using gears::CompareRhiDrawPacket;
    using gears::CompareRhiDrawRenderTargetState;
    using gears::CompareRhiDrawVertexState;
    using gears::RhiBindingEvidenceResult;
    using gears::RhiBindingStateEvidence;
    using gears::RhiDrawEvidenceResult;
    using gears::RhiDrawPacketEvidence;
    using gears::RhiPresentEvidenceResult;
    using gears::RhiPresentPacketEvidence;
    using gears::RhiRendererDrawEvidenceResult;
    using gears::RhiRendererDrawInput;
    using gears::RhiResolveEvidenceResult;
    using gears::RhiResolvePacketEvidence;
    using gears::RhiResourceConstructionEvidence;
    using gears::RhiResourceLifetimeEvidence;
    using gears::RhiResourceLifetimeEvidenceResult;
    using gears::RhiResourceLifetimeOperation;
    using gears::RhiSemanticBinding;
    using gears::RhiSemanticBindingKind;
    using gears::RhiSemanticDraw;
    using gears::RhiSemanticDrawKind;
    using gears::RhiSemanticPresent;
    using gears::RhiSemanticResolve;
    using gears::RhiSemanticResourceConstruction;
    using gears::RhiSemanticResourceConstructionKind;
    using gears::RhiSemanticResourceLifetime;
    using gears::RhiSemanticVertexStreamReset;
    using gears::RhiVertexStreamResetEvidence;
    using gears::RhiVertexStreamResetEvidenceResult;

    constexpr std::uint64_t kVertexShaderHash = 0x1122334455667788ull;
    constexpr std::uint64_t kAlternateVertexShaderHash = 0x8877665544332211ull;
    constexpr std::uint64_t kPixelShaderHash = 0x123456789ABCDEF0ull;
    const RhiSemanticBinding semanticVertexShader{
        .kind = RhiSemanticBindingKind::VertexShader,
        .object = 0x40107000,
        .shaderModules = {{.guestAddress = 0x10000, .sizeBytes = 48, .hash = kVertexShaderHash}},
    };
    const RhiSemanticBinding semanticPixelShader{
        .kind = RhiSemanticBindingKind::PixelShader,
        .object = 0x40107800,
        .shaderModules = {{.guestAddress = 0x10200, .sizeBytes = 72, .hash = kPixelShaderHash}},
    };

    const RhiSemanticDraw autoIndexed{
        .kind = RhiSemanticDrawKind::BoundVertices,
        .primitiveType = 4,
        .elementCount = 300,
    };
    const RhiDrawPacketEvidence matchingAuto{
        .present = true,
        .packetGuestAddress = 0xA0010000,
        .opcode = 0x22,
        .primitiveType = 4,
        .sourceSelect = 2,
        .elementCount = 300,
        .vertexStreamsPresent = true,
        .renderTargetsPresent = true,
    };
    assert(CompareRhiDrawPacket(autoIndexed, matchingAuto) == RhiDrawEvidenceResult::Match);

    RhiDrawPacketEvidence altered = matchingAuto;
    altered.elementCount = 299;
    assert(CompareRhiDrawPacket(autoIndexed, altered) == RhiDrawEvidenceResult::Mismatch);
    altered = matchingAuto;
    altered.present = false;
    assert(CompareRhiDrawPacket(autoIndexed, altered) == RhiDrawEvidenceResult::Missing);

    const RhiSemanticDraw indexed{
        .kind = RhiSemanticDrawKind::BoundIndices,
        .primitiveType = 3,
        .elementCount = 42,
        .startIndex = 8,
        .indexData = {.guestAddress = 0x2010, .sizeBytes = 84},
        .indexBufferViewPresent = true,
        .indexBuffer =
            {
                .allocation = {.guestAddress = 0x2000, .sizeBytes = 256},
                .elementStrideBytes = 2,
                .endianSwap = 1,
            },
    };
    const RhiDrawPacketEvidence matchingIndexed{
        .present = true,
        .packetGuestAddress = 0xA0020000,
        .opcode = 0x22,
        .primitiveType = 3,
        .sourceSelect = 0,
        .elementCount = 42,
        .indexData = {.guestAddress = 0x2010, .sizeBytes = 84},
        .indexDataPresent = true,
        .indexStrideBytes = 2,
        .indexEndianSwap = 1,
        .vertexStreamsPresent = true,
        .renderTargetsPresent = true,
    };
    assert(CompareRhiDrawPacket(indexed, matchingIndexed) == RhiDrawEvidenceResult::Match);
    altered = matchingIndexed;
    altered.sourceSelect = 2;
    assert(CompareRhiDrawPacket(indexed, altered) == RhiDrawEvidenceResult::Mismatch);
    altered = matchingIndexed;
    altered.indexData.guestAddress += 2;
    assert(CompareRhiDrawPacket(indexed, altered) == RhiDrawEvidenceResult::Mismatch);
    altered = matchingIndexed;
    altered.indexStrideBytes = 4;
    assert(CompareRhiDrawPacket(indexed, altered) == RhiDrawEvidenceResult::Mismatch);
    altered = matchingIndexed;
    altered.indexDataPresent = false;
    assert(CompareRhiDrawPacket(indexed, altered) == RhiDrawEvidenceResult::Missing);

    const RhiSemanticDraw transient{
        .kind = RhiSemanticDrawKind::TransientVerticesAndIndices,
        .primitiveType = 4,
        .elementCount = 6,
        .vertexStrideBytes = 32,
        .indexFormatFlags = 0,
        .vertexData = {.guestAddress = 0x1000, .sizeBytes = 128},
        .indexData = {.guestAddress = 0x1080, .sizeBytes = 12},
    };
    const RhiDrawPacketEvidence matchingTransient{
        .present = true,
        .opcode = 0x22,
        .primitiveType = 4,
        .sourceSelect = 0,
        .elementCount = 6,
        .transientDataPresent = true,
        .vertexData = {.guestAddress = 0x1000, .sizeBytes = 128},
        .indexData = {.guestAddress = 0x1080, .sizeBytes = 12},
    };
    assert(CompareRhiDrawPacket(transient, matchingTransient) == RhiDrawEvidenceResult::Match);
    RhiDrawPacketEvidence alteredTransient = matchingTransient;
    alteredTransient.vertexData.sizeBytes -= 4;
    assert(CompareRhiDrawPacket(transient, alteredTransient) == RhiDrawEvidenceResult::Mismatch);
    alteredTransient = matchingTransient;
    alteredTransient.indexData.guestAddress += 4;
    assert(CompareRhiDrawPacket(transient, alteredTransient) == RhiDrawEvidenceResult::Mismatch);
    alteredTransient = matchingTransient;
    alteredTransient.transientDataPresent = false;
    assert(CompareRhiDrawPacket(transient, alteredTransient) == RhiDrawEvidenceResult::Missing);

    const RhiSemanticBinding textureBinding{
        .kind = RhiSemanticBindingKind::Texture,
        .slot = 3,
        .object = 0x40102030,
    };
    const RhiBindingStateEvidence matchingBinding{
        .present = true,
        .observedObject = 0x40102030,
        .descriptor = {1, 2, 3, 4, 5, 6},
        .descriptorDwords = 6,
    };
    assert(CompareRhiBindingState(textureBinding, matchingBinding) ==
           RhiBindingEvidenceResult::Match);
    RhiBindingStateEvidence alteredBinding = matchingBinding;
    alteredBinding.observedObject ^= 4;
    assert(CompareRhiBindingState(textureBinding, alteredBinding) ==
           RhiBindingEvidenceResult::Mismatch);
    alteredBinding = matchingBinding;
    alteredBinding.present = false;
    assert(CompareRhiBindingState(textureBinding, alteredBinding) ==
           RhiBindingEvidenceResult::Missing);

    const RhiSemanticBinding colorTargetBinding{
        .kind = RhiSemanticBindingKind::ColorRenderTarget,
        .slot = 2,
        .object = 0x40104000,
        .descriptor = {0x00097813},
        .descriptorDwords = 1,
    };
    const RhiBindingStateEvidence matchingColorTarget{
        .present = true,
        .observedObject = 0x40104000,
        .descriptor = {0x00097813},
        .descriptorDwords = 1,
    };
    assert(CompareRhiBindingState(colorTargetBinding, matchingColorTarget) ==
           RhiBindingEvidenceResult::Match);
    RhiBindingStateEvidence alteredColorTarget = matchingColorTarget;
    alteredColorTarget.descriptor[0] ^= 1;
    assert(CompareRhiBindingState(colorTargetBinding, alteredColorTarget) ==
           RhiBindingEvidenceResult::Mismatch);

    const RhiSemanticBinding vertexStreamBinding{
        .kind = RhiSemanticBindingKind::VertexStream,
        .slot = 3,
        .object = 0x40102000,
        .bufferViewPresent = true,
        .bufferView =
            {
                .allocation = {.guestAddress = 0x00102040, .sizeBytes = 0x2FC0},
                .elementStrideBytes = 20,
            },
    };
    const RhiBindingStateEvidence matchingVertexStream{
        .present = true,
        .observedObject = 0x40102000,
        .bufferViewPresent = true,
        .bufferView =
            {
                .allocation = {.guestAddress = 0x00102040, .sizeBytes = 0x2FC0},
                .elementStrideBytes = 20,
            },
    };
    assert(CompareRhiBindingState(vertexStreamBinding, matchingVertexStream) ==
           RhiBindingEvidenceResult::Match);
    RhiBindingStateEvidence alteredVertexStream = matchingVertexStream;
    alteredVertexStream.bufferView.elementStrideBytes = 16;
    assert(CompareRhiBindingState(vertexStreamBinding, alteredVertexStream) ==
           RhiBindingEvidenceResult::Mismatch);

    const gears::RhiSemanticDrawState boundDrawState{
        .draw = indexed,
        .vertexStreams = {{.slot = 3,
                           .object = vertexStreamBinding.object,
                           .view = vertexStreamBinding.bufferView}},
        .pixelShader = semanticPixelShader,
        .vertexShader = semanticVertexShader,
    };
    RhiDrawPacketEvidence matchingVertexState = matchingIndexed;
    matchingVertexState.vertexStreams = boundDrawState.vertexStreams;
    assert(CompareRhiDrawVertexState(boundDrawState, matchingVertexState) ==
           RhiDrawEvidenceResult::Match);
    matchingVertexState.vertexStreams[0].view.elementStrideBytes = 16;
    assert(CompareRhiDrawVertexState(boundDrawState, matchingVertexState) ==
           RhiDrawEvidenceResult::Mismatch);
    matchingVertexState.vertexStreamsPresent = false;
    assert(CompareRhiDrawVertexState(boundDrawState, matchingVertexState) ==
           RhiDrawEvidenceResult::Missing);

    const gears::RhiSemanticDrawState targetDrawState{
        .draw = indexed,
        .renderTargets = {{.slot = 2, .object = colorTargetBinding.object}},
    };
    RhiDrawPacketEvidence matchingTargetState = matchingIndexed;
    matchingTargetState.renderTargets = targetDrawState.renderTargets;
    assert(CompareRhiDrawRenderTargetState(targetDrawState, matchingTargetState) ==
           RhiDrawEvidenceResult::Match);
    matchingTargetState.renderTargets[0].object ^= 1;
    assert(CompareRhiDrawRenderTargetState(targetDrawState, matchingTargetState) ==
           RhiDrawEvidenceResult::Mismatch);
    matchingTargetState.renderTargetsPresent = false;
    assert(CompareRhiDrawRenderTargetState(targetDrawState, matchingTargetState) ==
           RhiDrawEvidenceResult::Missing);

    const RhiSemanticBinding depthTargetBinding{
        .kind = RhiSemanticBindingKind::DepthStencilTarget,
        .object = 0x40104800,
        .descriptor = {0x000002D0, 0x00000001},
        .descriptorDwords = 2,
    };
    const RhiBindingStateEvidence matchingDepthTarget{
        .present = true,
        .observedObject = 0x40104800,
        .descriptor = {0x000002D0, 0x00000001},
        .descriptorDwords = 2,
    };
    assert(CompareRhiBindingState(depthTargetBinding, matchingDepthTarget) ==
           RhiBindingEvidenceResult::Match);
    RhiBindingStateEvidence alteredDepthTarget = matchingDepthTarget;
    alteredDepthTarget.descriptor[1] ^= 1;
    assert(CompareRhiBindingState(depthTargetBinding, alteredDepthTarget) ==
           RhiBindingEvidenceResult::Mismatch);

    const gears::RhiSemanticDrawState normalizedTargetDraw{
        .draw = autoIndexed,
        .renderTargets =
            {{.slot = 0,
              .object = 0x40104000,
              .normalizedStatePresent = true,
              .normalizedState = {.base = 0x2D0, .format = 12, .colorExponentBias = -2}},
             {.depthStencil = true,
              .object = 0x40104800,
              .normalizedStatePresent = true,
              .normalizedState = {.base = 0x5A0, .format = 1}}},
        .pixelShader = semanticPixelShader,
        .vertexShader = semanticVertexShader,
        .surfaceStatePresent = true,
        .surfaceState = {.pitch = 1280, .msaaSamples = 1},
    };
    const RhiRendererDrawInput matchingRendererTargets{
        .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized,
        .primitiveType = 4,
        .elementCount = 300,
        .vertexShaderHash = kVertexShaderHash,
        .pixelShaderHash = kPixelShaderHash,
        .targetStatePresent = true,
        .colorTargetStatePresent = true,
        .depthTargetStatePresent = true,
        .colorTarget = {.base = 0x2D0, .format = 12, .colorExponentBias = -2},
        .depthTarget = {.base = 0x5A0, .format = 1},
        .surfaceState = {.pitch = 1280, .msaaSamples = 1},
    };
    assert(gears::CompareRhiRendererDrawInput(normalizedTargetDraw, matchingRendererTargets) ==
           RhiRendererDrawEvidenceResult::Match);
    constexpr std::array targetMismatchReasons{
        gears::RhiRendererDrawEvidenceReason::ColorTargetState,
        gears::RhiRendererDrawEvidenceReason::ColorTargetState,
        gears::RhiRendererDrawEvidenceReason::ColorTargetState,
        gears::RhiRendererDrawEvidenceReason::DepthTargetState,
        gears::RhiRendererDrawEvidenceReason::DepthTargetState,
        gears::RhiRendererDrawEvidenceReason::SurfaceState,
        gears::RhiRendererDrawEvidenceReason::SurfaceState,
    };
    for (const auto alteredTarget : {0, 1, 2, 3, 4, 5, 6})
    {
        RhiRendererDrawInput renderer = matchingRendererTargets;
        switch (alteredTarget)
        {
        case 0:
            renderer.colorTarget.base ^= 1;
            break;
        case 1:
            renderer.colorTarget.format ^= 1;
            break;
        case 2:
            renderer.colorTarget.colorExponentBias += 1;
            break;
        case 3:
            renderer.depthTarget.base ^= 1;
            break;
        case 4:
            renderer.depthTarget.format ^= 1;
            break;
        case 5:
            renderer.surfaceState.pitch += 1;
            break;
        case 6:
            renderer.surfaceState.msaaSamples ^= 1;
            break;
        }
        const gears::RhiRendererDrawEvidence evidence =
            gears::InspectRhiRendererDrawInput(normalizedTargetDraw, renderer);
        assert(evidence.result == RhiRendererDrawEvidenceResult::Mismatch);
        assert(evidence.reason == targetMismatchReasons[alteredTarget]);
    }
    RhiRendererDrawInput missingColorTarget = matchingRendererTargets;
    missingColorTarget.colorTargetStatePresent = false;
    assert(gears::CompareRhiRendererDrawInput(normalizedTargetDraw, missingColorTarget) ==
           RhiRendererDrawEvidenceResult::Missing);
    assert(gears::InspectRhiRendererDrawInput(normalizedTargetDraw, missingColorTarget).reason ==
           gears::RhiRendererDrawEvidenceReason::ColorTargetStateUnavailable);
    auto noSemanticDepthTarget = normalizedTargetDraw;
    noSemanticDepthTarget.renderTargets.erase(noSemanticDepthTarget.renderTargets.begin() + 1);
    assert(
        gears::InspectRhiRendererDrawInput(noSemanticDepthTarget, matchingRendererTargets).result ==
        RhiRendererDrawEvidenceResult::Match);
    RhiRendererDrawInput missingDepthTarget = matchingRendererTargets;
    missingDepthTarget.depthTargetStatePresent = false;
    const gears::RhiRendererDrawEvidence missingDepthEvidence =
        gears::InspectRhiRendererDrawInput(normalizedTargetDraw, missingDepthTarget);
    assert(missingDepthEvidence.result == RhiRendererDrawEvidenceResult::Missing);
    assert(missingDepthEvidence.reason ==
           gears::RhiRendererDrawEvidenceReason::DepthTargetStateUnavailable);
    auto missingDescriptor = normalizedTargetDraw;
    missingDescriptor.renderTargets[0].normalizedStatePresent = false;
    assert(gears::CompareRhiRendererDrawInput(missingDescriptor, matchingRendererTargets) ==
           RhiRendererDrawEvidenceResult::Missing);
    assert(gears::InspectRhiRendererDrawInput(missingDescriptor, matchingRendererTargets).reason ==
           gears::RhiRendererDrawEvidenceReason::ColorTargetStateMissing);
    auto unsupportedMrt = normalizedTargetDraw;
    unsupportedMrt.renderTargets.push_back({.slot = 1,
                                            .object = 0x40105000,
                                            .normalizedStatePresent = true,
                                            .normalizedState = {.base = 0x600, .format = 3}});
    assert(gears::CompareRhiRendererDrawInput(unsupportedMrt, matchingRendererTargets) ==
           RhiRendererDrawEvidenceResult::Missing);
    assert(gears::InspectRhiRendererDrawInput(unsupportedMrt, matchingRendererTargets).reason ==
           gears::RhiRendererDrawEvidenceReason::UnsupportedColorTargetSlot);

    const gears::RhiSemanticDrawState textureDrawState{
        .draw = autoIndexed,
        .textures = {{.kind = RhiSemanticBindingKind::Texture,
                      .slot = 3,
                      .object = 0x40106000,
                      .descriptor = {2, 0x10, 0x20, 0x30, 0x40, 0x50},
                      .descriptorDwords = 6}},
        .pixelShader = semanticPixelShader,
        .vertexShader = semanticVertexShader,
    };
    RhiRendererDrawInput matchingRendererTexture{
        .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized,
        .primitiveType = 4,
        .elementCount = 300,
        .vertexShaderHash = kVertexShaderHash,
        .pixelShaderHash = kPixelShaderHash,
        .textureFetchStatePresent = true,
    };
    matchingRendererTexture.textureFetches[3] = {2, 0x10, 0x20, 0x30, 0x40, 0x50};
    assert(gears::CompareRhiRendererDrawInput(textureDrawState, matchingRendererTexture) ==
           RhiRendererDrawEvidenceResult::Match);
    for (std::size_t dword = 0; dword < gears::draw::kNativeTextureFetchDwords; ++dword)
    {
        RhiRendererDrawInput alteredTexture = matchingRendererTexture;
        alteredTexture.textureFetches[3][dword] ^= 1;
        const gears::RhiRendererDrawEvidence evidence =
            gears::InspectRhiRendererDrawInput(textureDrawState, alteredTexture);
        assert(evidence.result == RhiRendererDrawEvidenceResult::Mismatch);
        assert(evidence.reason == gears::RhiRendererDrawEvidenceReason::TextureState);
        assert(evidence.textureMismatchPresent);
        assert(evidence.textureSlot == 3);
        assert(evidence.textureDword == dword);
        assert(evidence.semanticTextureValue == textureDrawState.textures[0].descriptor[dword]);
        assert(evidence.rendererTextureValue == alteredTexture.textureFetches[3][dword]);
    }
    RhiRendererDrawInput missingRendererTexture = matchingRendererTexture;
    missingRendererTexture.textureFetchStatePresent = false;
    assert(gears::InspectRhiRendererDrawInput(textureDrawState, missingRendererTexture).reason ==
           gears::RhiRendererDrawEvidenceReason::RendererTextureStateMissing);
    auto missingSemanticTexture = textureDrawState;
    missingSemanticTexture.textures[0].descriptorDwords = 0;
    assert(gears::InspectRhiRendererDrawInput(missingSemanticTexture, matchingRendererTexture)
               .reason == gears::RhiRendererDrawEvidenceReason::SemanticTextureStateMissing);
    auto unsupportedTexture = textureDrawState;
    unsupportedTexture.textures[0].slot = gears::draw::kNativeTextureFetchSlots;
    assert(gears::InspectRhiRendererDrawInput(unsupportedTexture, matchingRendererTexture).reason ==
           gears::RhiRendererDrawEvidenceReason::UnsupportedTextureSlot);
    auto duplicateTexture = textureDrawState;
    duplicateTexture.textures.push_back(duplicateTexture.textures[0]);
    assert(gears::InspectRhiRendererDrawInput(duplicateTexture, matchingRendererTexture).reason ==
           gears::RhiRendererDrawEvidenceReason::DuplicateTextureSlot);

    const RhiSemanticBinding indexBufferBinding{
        .kind = RhiSemanticBindingKind::IndexBuffer,
        .object = 0x40105000,
        .bufferViewPresent = true,
        .bufferView =
            {
                .allocation = {.guestAddress = 0x00978000, .sizeBytes = 0x1000},
                .elementStrideBytes = 4,
                .endianSwap = 2,
            },
    };
    const RhiBindingStateEvidence matchingIndexBuffer{
        .present = true,
        .observedObject = 0x40105000,
        .bufferViewPresent = true,
        .bufferView =
            {
                .allocation = {.guestAddress = 0x00978000, .sizeBytes = 0x1000},
                .elementStrideBytes = 4,
                .endianSwap = 2,
            },
    };
    assert(CompareRhiBindingState(indexBufferBinding, matchingIndexBuffer) ==
           RhiBindingEvidenceResult::Match);
    RhiBindingStateEvidence alteredIndexBuffer = matchingIndexBuffer;
    alteredIndexBuffer.bufferView.allocation.sizeBytes -= 4;
    assert(CompareRhiBindingState(indexBufferBinding, alteredIndexBuffer) ==
           RhiBindingEvidenceResult::Mismatch);

    const RhiSemanticPresent present{
        .frameSequence = 17,
        .frontBuffer = 0xA0311000,
        .fetchDescriptor = {10, 20, 30, 40, 50, 60},
    };
    const RhiPresentPacketEvidence matchingPresent{
        .present = true,
        .framingValid = true,
        .frameSequence = 17,
        .frontBuffer = 0xA0311000,
        .fetchDescriptor = {10, 20, 30, 40, 50, 60},
    };
    assert(gears::CompareRhiPresentPacket(present, matchingPresent) ==
           RhiPresentEvidenceResult::Match);
    RhiPresentPacketEvidence alteredPresent = matchingPresent;
    alteredPresent.fetchDescriptor[4] ^= 1;
    assert(gears::CompareRhiPresentPacket(present, alteredPresent) ==
           RhiPresentEvidenceResult::Mismatch);
    alteredPresent = matchingPresent;
    alteredPresent.present = false;
    assert(gears::CompareRhiPresentPacket(present, alteredPresent) ==
           RhiPresentEvidenceResult::Missing);

    const RhiSemanticResolve resolve{
        .sourceDepthStencil = false,
        .sourceSlot = 0,
        .sourceObject = 0x40106000,
        .destinationObject = 0x40107000,
        .destinationAddress = 0x0BCC0000,
        .destinationPitch = 1280,
        .destinationHeight = 208,
        .destinationFormat = 23,
    };
    const RhiResolvePacketEvidence matchingResolve{
        .present = true,
        .observedSourceObject = 0x40106000,
        .destinationAddress = 0x0BCC0000,
        .destinationPitch = 1280,
        .destinationHeight = 208,
        .drawOpcode = 0x22,
        .primitiveType = 8,
        .sourceSelect = 2,
        .elementCount = 3,
    };
    assert(gears::CompareRhiResolvePacket(resolve, matchingResolve) ==
           RhiResolveEvidenceResult::Match);
    RhiResolvePacketEvidence alteredResolve = matchingResolve;
    alteredResolve.destinationAddress += 0x1000;
    assert(gears::CompareRhiResolvePacket(resolve, alteredResolve) ==
           RhiResolveEvidenceResult::Mismatch);
    alteredResolve = matchingResolve;
    alteredResolve.present = false;
    assert(gears::CompareRhiResolvePacket(resolve, alteredResolve) ==
           RhiResolveEvidenceResult::Missing);

    const gears::RhiSemanticDrawState rendererAutoState{
        .draw = autoIndexed,
        .pixelShader = semanticPixelShader,
        .vertexShader = semanticVertexShader,
    };
    const RhiRendererDrawInput rendererAuto{
        .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized,
        .primitiveType = 4,
        .elementCount = 300,
        .indexed = false,
        .vertexShaderHash = kVertexShaderHash,
        .pixelShaderHash = kPixelShaderHash,
    };
    assert(gears::CompareRhiRendererDrawInput(rendererAutoState, rendererAuto) ==
           RhiRendererDrawEvidenceResult::Match);
    RhiRendererDrawInput alteredRenderer = rendererAuto;
    alteredRenderer.elementCount = 299;
    assert(gears::CompareRhiRendererDrawInput(rendererAutoState, alteredRenderer) ==
           RhiRendererDrawEvidenceResult::Mismatch);

    RhiRendererDrawInput alternateVertexRenderer = rendererAuto;
    alternateVertexRenderer.vertexShaderHash = kAlternateVertexShaderHash;
    const gears::RhiRendererDrawEvidence wrongVertexShader =
        gears::InspectRhiRendererDrawInput(rendererAutoState, alternateVertexRenderer);
    assert(wrongVertexShader.result == RhiRendererDrawEvidenceResult::Mismatch);
    assert(wrongVertexShader.reason == gears::RhiRendererDrawEvidenceReason::VertexShaderModule);

    gears::RhiSemanticDrawState ambiguousVertexState = rendererAutoState;
    ambiguousVertexState.vertexShader->shaderModules.push_back(
        {.guestAddress = 0x10100, .sizeBytes = 60, .hash = kAlternateVertexShaderHash});
    const gears::RhiRendererDrawEvidence ambiguousVertex =
        gears::InspectRhiRendererDrawInput(ambiguousVertexState, alternateVertexRenderer);
    assert(ambiguousVertex.result == RhiRendererDrawEvidenceResult::Missing);
    assert(ambiguousVertex.reason ==
           gears::RhiRendererDrawEvidenceReason::SemanticVertexShaderModulesAmbiguous);

    RhiRendererDrawInput wrongPixelRenderer = rendererAuto;
    wrongPixelRenderer.pixelShaderHash ^= 1;
    const gears::RhiRendererDrawEvidence wrongPixelShader =
        gears::InspectRhiRendererDrawInput(rendererAutoState, wrongPixelRenderer);
    assert(wrongPixelShader.result == RhiRendererDrawEvidenceResult::Mismatch);
    assert(wrongPixelShader.reason == gears::RhiRendererDrawEvidenceReason::PixelShaderModule);

    RhiRendererDrawInput rendererIndexed{
        .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized,
        .primitiveType = 3,
        .elementCount = 42,
        .indexed = true,
        .indexEndian = 1,
        .indexGuestBase = 0x2010,
        .vertexShaderHash = kVertexShaderHash,
        .pixelShaderHash = kPixelShaderHash,
    };
    assert(gears::CompareRhiRendererDrawInput(boundDrawState, rendererIndexed) ==
           RhiRendererDrawEvidenceResult::Match);
    rendererIndexed.indexGuestBase |= 0xA0000000;
    assert(gears::InspectRhiRendererDrawInput(boundDrawState, rendererIndexed).result ==
           RhiRendererDrawEvidenceResult::Match);
    rendererIndexed.indexGuestBase += 2;
    const gears::RhiRendererDrawEvidence wrongIndexAddress =
        gears::InspectRhiRendererDrawInput(boundDrawState, rendererIndexed);
    assert(wrongIndexAddress.result == RhiRendererDrawEvidenceResult::Mismatch);
    assert(wrongIndexAddress.reason == gears::RhiRendererDrawEvidenceReason::IndexAddress);
    rendererIndexed.indexGuestBase = 0x2010;
    rendererIndexed.outcome = gears::draw::NativeDrawMaterializationOutcome::Refused;
    assert(gears::CompareRhiRendererDrawInput(boundDrawState, rendererIndexed) ==
           RhiRendererDrawEvidenceResult::Missing);

    const RhiSemanticResourceLifetime addReference{
        .operation = RhiResourceLifetimeOperation::AddReference,
        .object = 0x40108000,
        .rawFlags = 0x40000004,
        .resourceType = 4,
        .backingObject = 0x40109000,
        .previousReferenceCount = 2,
    };
    const RhiResourceLifetimeEvidence matchingAddReference{
        .present = true,
        .returnedReferenceCount = 3,
    };
    assert(gears::CompareRhiResourceLifetime(addReference, matchingAddReference) ==
           RhiResourceLifetimeEvidenceResult::Match);
    RhiResourceLifetimeEvidence alteredLifetime = matchingAddReference;
    alteredLifetime.returnedReferenceCount = 4;
    assert(gears::CompareRhiResourceLifetime(addReference, alteredLifetime) ==
           RhiResourceLifetimeEvidenceResult::Mismatch);
    alteredLifetime.present = false;
    assert(gears::CompareRhiResourceLifetime(addReference, alteredLifetime) ==
           RhiResourceLifetimeEvidenceResult::Missing);

    const RhiSemanticResourceLifetime release{
        .operation = RhiResourceLifetimeOperation::Release,
        .object = 0x40108000,
        .rawFlags = 0x40000004,
        .resourceType = 4,
        .backingObject = 0x40109000,
        .previousReferenceCount = 1,
    };
    const RhiResourceLifetimeEvidence matchingRelease{
        .present = true,
        .returnedReferenceCount = 0,
    };
    assert(gears::CompareRhiResourceLifetime(release, matchingRelease) ==
           RhiResourceLifetimeEvidenceResult::Match);

    const RhiSemanticResourceConstruction construction{
        .kind = RhiSemanticResourceConstructionKind::OwnedBacking,
        .requestedBytes = 120,
        .resourceFlags = 0,
        .allocationFlags = 0,
    };
    const RhiResourceConstructionEvidence matchingConstruction{
        .present = true,
        .object = 0x4010B000,
        .objectWords = {0x30100001, 1, 0xFFFF0000, 0x6010B003, 120},
    };

    const RhiSemanticVertexStreamReset vertexStreamReset{.firstSlot = 0, .slotCount = 16};
    const RhiVertexStreamResetEvidence matchingVertexStreamReset{.present = true};
    assert(gears::CompareRhiVertexStreamReset(vertexStreamReset, matchingVertexStreamReset) ==
           RhiVertexStreamResetEvidenceResult::Match);
    const RhiVertexStreamResetEvidence alteredVertexStreamReset{
        .present = true,
        .activeStreams = {{.slot = 1, .object = 0x4010A000}},
    };
    assert(gears::CompareRhiVertexStreamReset(vertexStreamReset, alteredVertexStreamReset) ==
           RhiVertexStreamResetEvidenceResult::Mismatch);
    assert(gears::CompareRhiVertexStreamReset(vertexStreamReset, {}) ==
           RhiVertexStreamResetEvidenceResult::Missing);

    assert(setenv("GEARS_NATIVE_RHI_OBSERVE", "1", 1) == 0);
    lucent::config::set_prefix("GEARS_");
    const RhiSemanticBinding activeColorTarget{
        .kind = RhiSemanticBindingKind::ColorRenderTarget,
        .slot = 0,
        .object = 0x40104000,
        .descriptor = {0x000302D0},
        .descriptorDwords = 1,
    };
    const RhiBindingStateEvidence activeColorTargetState{
        .present = true,
        .observedObject = activeColorTarget.object,
        .descriptor = activeColorTarget.descriptor,
        .descriptorDwords = 1,
        .targetStatePresent = true,
        .targetState = {.base = 0x2D0, .format = 3},
        .surfaceStatePresent = true,
        .surfaceState = {.pitch = 1280, .msaaSamples = 0},
    };
    const gears::RhiSemanticColorWriteState observedGammaWrite{
        .requested = 1,
        .targetPresent = true,
        .target = {.slot = 0,
                   .object = activeColorTarget.object,
                   .descriptor = {0x000C02D0},
                   .descriptorDwords = 1,
                   .normalizedStatePresent = true,
                   .normalizedState = {.base = 0x2D0, .format = 12}},
        .surfaceStatePresent = true,
        .surfaceState = activeColorTargetState.surfaceState,
    };
    gears::ObserveRhiSemanticDraw(autoIndexed, matchingAuto);
    gears::ObserveRhiSemanticBinding(textureBinding, matchingBinding);
    gears::ObserveRhiSemanticResourceLifetime(release, matchingRelease);
    gears::ObserveRhiSemanticResourceConstruction(construction, matchingConstruction);
    gears::ObserveRhiSemanticVertexStreamReset(vertexStreamReset, matchingVertexStreamReset);
    gears::ObserveRhiSemanticResolve(resolve, matchingResolve);
    altered = matchingIndexed;
    altered.sourceSelect = 2;
    gears::ObserveRhiSemanticDraw(indexed, altered);
    gears::ObserveRhiSemanticBinding(activeColorTarget, activeColorTargetState);
    gears::ObserveRhiSemanticColorWriteState(observedGammaWrite);
    gears::ObserveRhiSemanticPresent(present, matchingPresent);
    const gears::RhiSemanticFrame frame = gears::SealRhiSemanticFrame(17);
    assert(frame.frameSequence == 17);
    assert(frame.events.size() == 10);
    assert(frame.draws == 2);
    assert(frame.bindings == 2);
    assert(frame.resourceLifetimeCalls == 1);
    assert(frame.resourceRetirements == 1);
    assert(frame.resourceConstructions == 1);
    assert(frame.vertexStreamResets == 1);
    assert(frame.colorWriteStates == 1);
    assert(frame.resolves == 1);
    assert(frame.presents == 1);
    for (std::size_t index = 1; index < frame.events.size(); ++index)
        assert(frame.events[index - 1].sequence + 1 == frame.events[index].sequence);
    assert(std::holds_alternative<gears::RhiObservedDraw>(frame.events[0].payload));
    assert(std::holds_alternative<gears::RhiObservedBinding>(frame.events[1].payload));
    assert(std::holds_alternative<gears::RhiObservedResourceLifetime>(frame.events[2].payload));
    assert(std::holds_alternative<gears::RhiObservedResourceConstruction>(frame.events[3].payload));
    assert(std::holds_alternative<gears::RhiObservedVertexStreamReset>(frame.events[4].payload));
    assert(std::holds_alternative<gears::RhiObservedResolve>(frame.events[5].payload));
    assert(std::holds_alternative<gears::RhiObservedDraw>(frame.events[6].payload));
    assert(std::holds_alternative<gears::RhiObservedBinding>(frame.events[7].payload));
    assert(std::holds_alternative<gears::RhiObservedColorWriteState>(frame.events[8].payload));
    assert(std::holds_alternative<gears::RhiObservedPresent>(frame.events[9].payload));
    assert(std::get<gears::RhiObservedDraw>(frame.events[0].payload).state.draw.elementCount ==
           300);
    assert(std::get<gears::RhiObservedBinding>(frame.events[1].payload).binding.slot == 3);
    assert(std::get<gears::RhiObservedResourceLifetime>(frame.events[2].payload)
               .retained.returnedReferenceCount == 0);
    assert(std::get<gears::RhiObservedResourceConstruction>(frame.events[3].payload)
               .retained.objectWords[4] == 120);
    assert(
        std::get<gears::RhiObservedResolve>(frame.events[5].payload).resolve.destinationAddress ==
        0x0BCC0000);
    assert(std::get<gears::RhiObservedDraw>(frame.events[6].payload).state.draw.elementCount == 42);
    assert(std::get<gears::RhiObservedColorWriteState>(frame.events[8].payload)
               .state.target.normalizedState.format == 12);
    assert(std::get<gears::RhiObservedPresent>(frame.events[9].payload).present.frontBuffer ==
           0xA0311000);
    assert(frame.matched == 1);
    assert(frame.missing == 0);
    assert(frame.mismatched == 1);
    assert(frame.bindingsMatched == 2);
    assert(frame.bindingsMissing == 0);
    assert(frame.bindingsMismatched == 0);
    assert(frame.resourceLifetimeMatched == 1);
    assert(frame.resourceLifetimeMissing == 0);
    assert(frame.resourceLifetimeMismatched == 0);
    assert(frame.vertexStreamResetsMatched == 1);
    assert(frame.vertexStreamResetsMissing == 0);
    assert(frame.vertexStreamResetsMismatched == 0);
    assert(frame.colorWriteStatesMatched == 1);
    assert(frame.colorWriteStatesMissing == 0);
    assert(frame.colorWriteStatesMismatched == 0);
    assert(frame.resolvesMatched == 1);
    assert(frame.resolvesMissing == 0);
    assert(frame.resolvesMismatched == 0);
    assert(frame.presentsMatched == 1);
    assert(frame.presentsMissing == 0);
    assert(frame.presentsMismatched == 0);
    const gears::RhiRendererFrameComparison missingRenderer = gears::CompareRhiRendererDraws(
        frame, {.status = gears::draw::NativeFrameMaterializationStatus::Dropped});
    assert(missingRenderer.semanticDraws == 2);
    assert(missingRenderer.missing == 2);
    gears::RhiRendererFrameInput wrongRendererInput{
        .draws =
            {
                {.sourceOrdinal = 0,
                 .packetGuestAddress = 0x00010000,
                 .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized,
                 .primitiveType = 4,
                 .elementCount = 299},
                {.sourceOrdinal = 1,
                 .packetGuestAddress = 0x00020000,
                 .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized,
                 .primitiveType = 3,
                 .elementCount = 42,
                 .indexed = true,
                 .indexEndian = 1,
                 .indexGuestBase = 0x2010},
                {.sourceOrdinal = 2,
                 .packetGuestAddress = 0x00030000,
                 .packetBufferBase = 0x00030000,
                 .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized,
                 .primitiveType = 4,
                 .elementCount = 6},
                {.sourceOrdinal = 3,
                 .packetGuestAddress = 0x00040000,
                 .packetBufferBase = 0x0003F000,
                 .packetFromIndirectBuffer = true,
                 .outcome = gears::draw::NativeDrawMaterializationOutcome::Refused},
            },
    };
    wrongRendererInput.draws[1].textureFetchStatePresent = true;
    wrongRendererInput.draws[1].textureFetches[3] = {1, 2, 3, 4, 5, 6};
    const gears::RhiRendererFrameComparison wrongRenderer =
        gears::CompareRhiRendererDraws(frame, wrongRendererInput);
    assert(wrongRenderer.matched == 1);
    assert(wrongRenderer.mismatched == 1);
    assert(wrongRenderer.unmatchedRendererPackets == 2);
    assert(wrongRenderer.unmatchedRendererMaterializedPackets == 1);
    assert(wrongRenderer.unmatchedRendererRefusedPackets == 1);
    assert(wrongRenderer.unmatchedRendererRingPackets == 1);
    assert(wrongRenderer.unmatchedRendererIndirectPackets == 1);
    assert(wrongRenderer.firstUnmatchedRendererPacket == 0x00030000);
    assert(wrongRenderer.firstUnmatchedRendererBuffer == 0x00030000);
    assert(!wrongRenderer.firstUnmatchedRendererFromIndirectBuffer);
    assert(wrongRenderer.firstUnmatchedRendererOutcome ==
           gears::draw::NativeDrawMaterializationOutcome::Materialized);

    const gears::RhiRendererFrameInput unmatchedReplayInput{
        .draws = {{.sourceOrdinal = 0,
                   .packetGuestAddress = 0x00050000,
                   .packetBufferBase = 0x0004F000,
                   .packetFromIndirectBuffer = true,
                   .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized},
                  {.sourceOrdinal = 1,
                   .packetGuestAddress = 0x00050000,
                   .packetBufferBase = 0x00050000,
                   .outcome = gears::draw::NativeDrawMaterializationOutcome::Refused}},
    };
    const gears::RhiRendererFrameComparison unmatchedReplay =
        gears::CompareRhiRendererDraws({}, unmatchedReplayInput);
    assert(unmatchedReplay.unmatchedRendererPackets == 1);
    assert(unmatchedReplay.unmatchedRendererMaterializedPackets == 0);
    assert(unmatchedReplay.unmatchedRendererRefusedPackets == 0);
    assert(unmatchedReplay.unmatchedRendererMixedOutcomePackets == 1);
    assert(unmatchedReplay.unmatchedRendererIndirectPackets == 0);
    assert(unmatchedReplay.unmatchedRendererRingPackets == 0);
    assert(unmatchedReplay.unmatchedRendererInconsistentSourcePackets == 1);
    assert(unmatchedReplay.firstUnmatchedRendererMixedOutcome);
    assert(unmatchedReplay.firstUnmatchedRendererInconsistentSource);

    const gears::RhiSemanticFrame tileReplayFrame{
        .frameSequence = 30,
        .events = {frame.events[0]},
    };
    gears::RhiRendererFrameInput tileReplayInput{
        .draws = {{.sourceOrdinal = 0,
                   .packetGuestAddress = 0x00010000,
                   .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized,
                   .primitiveType = 4,
                   .elementCount = 300},
                  {.sourceOrdinal = 1,
                   .packetGuestAddress = 0x00010000,
                   .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized,
                   .primitiveType = 4,
                   .elementCount = 300}},
    };
    gears::RhiRendererFrameComparison tileReplay =
        gears::CompareRhiRendererDraws(tileReplayFrame, tileReplayInput);
    assert(tileReplay.matched == 1);
    assert(tileReplay.missing == 0);
    assert(tileReplay.mismatched == 0);
    tileReplayInput.draws[1].outcome = gears::draw::NativeDrawMaterializationOutcome::Refused;
    tileReplay = gears::CompareRhiRendererDraws(tileReplayFrame, tileReplayInput);
    assert(tileReplay.missing == 1);
    tileReplayInput.draws[1].outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized;
    tileReplayInput.draws[1].elementCount = 299;
    tileReplay = gears::CompareRhiRendererDraws(tileReplayFrame, tileReplayInput);
    assert(tileReplay.mismatched == 1);
    tileReplayInput.draws[1].packetGuestAddress = 0;
    tileReplayInput.draws[1].elementCount = 300;
    tileReplay = gears::CompareRhiRendererDraws(tileReplayFrame, tileReplayInput);
    assert(tileReplay.matched == 1);
    assert(tileReplay.unkeyedRendererDraws == 1);
    gears::RhiSemanticFrame packetCollisionFrame = tileReplayFrame;
    packetCollisionFrame.events.push_back(frame.events[0]);
    tileReplayInput.draws.resize(1);
    tileReplay = gears::CompareRhiRendererDraws(packetCollisionFrame, tileReplayInput);
    assert(tileReplay.matched == 1);
    assert(tileReplay.mismatched == 1);

    gears::ObserveRhiSemanticDraw(autoIndexed, matchingAuto);
    const gears::RhiSemanticFrame semanticFirst = gears::SealRhiSemanticFrame(18);
    assert(semanticFirst.draws == 1);
    gears::RhiRendererFrameInput semanticFirstRenderer{
        .draws = {{.sourceOrdinal = 0,
                   .packetGuestAddress = 0x00010000,
                   .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized,
                   .primitiveType = 4,
                   .elementCount = 300,
                   .textureFetchStatePresent = true,
                   .targetStatePresent = true,
                   .colorTargetStatePresent = true,
                   .colorTarget = {.base = 0x2D0, .format = 12},
                   .surfaceState = {.pitch = 1280, .msaaSamples = 0}}},
    };
    semanticFirstRenderer.draws[0].textureFetches[3] = {1, 2, 3, 4, 5, 6};
    const std::optional<gears::RhiRendererFrameComparison> joined =
        gears::PublishRhiRendererFrameInput(18, std::move(semanticFirstRenderer));
    assert(joined.has_value());
    assert(joined->matched == 1);
    assert(joined->missing == 0);
    assert(joined->mismatched == 0);
    const std::optional<gears::RhiRendererFrameComparison> postJoinDuplicate =
        gears::PublishRhiRendererFrameInput(18, {});
    assert(postJoinDuplicate.has_value());
    assert(postJoinDuplicate->duplicate);

    const std::optional<gears::RhiRendererFrameComparison> rendererFirstPending =
        gears::PublishRhiRendererFrameInput(
            19, {.draws = {{.sourceOrdinal = 0,
                            .packetGuestAddress = 0x00010000,
                            .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized,
                            .primitiveType = 4,
                            .elementCount = 300}}});
    assert(!rendererFirstPending.has_value());
    const std::optional<gears::RhiRendererFrameComparison> rendererFirstJoined =
        gears::ObserveRhiSemanticFrameSealed({.frameSequence = 19, .events = {frame.events[0]}});
    assert(rendererFirstJoined.has_value());
    assert(rendererFirstJoined->matched == 1);

    assert(!gears::ObserveRhiSemanticFrameSealed({.frameSequence = 20, .events = {frame.events[0]}})
                .has_value());
    assert(!gears::ObserveRhiSemanticFrameSealed({.frameSequence = 20, .events = {frame.events[0]}})
                .has_value());
    const std::optional<gears::RhiRendererFrameComparison> semanticDuplicate =
        gears::PublishRhiRendererFrameInput(20, {});
    assert(semanticDuplicate.has_value());
    assert(semanticDuplicate->duplicate);

    assert(!gears::PublishRhiRendererFrameInput(21, {}).has_value());
    assert(!gears::PublishRhiRendererFrameInput(21, {}).has_value());
    const std::optional<gears::RhiRendererFrameComparison> rendererDuplicate =
        gears::ObserveRhiSemanticFrameSealed({.frameSequence = 21});
    assert(rendererDuplicate.has_value());
    assert(rendererDuplicate->duplicate);

    assert(!gears::ObserveRhiSemanticFrameSealed({.frameSequence = 1000}).has_value());
    assert(!gears::ObserveRhiSemanticFrameSealed({.frameSequence = 1065}).has_value());
    const std::optional<gears::RhiRendererFrameComparison> expiredSemanticSide =
        gears::PublishRhiRendererFrameInput(1000, {});
    assert(expiredSemanticSide.has_value());
    assert(expiredSemanticSide->duplicate);

    assert(!gears::PublishRhiRendererFrameInput(2000, {}).has_value());
    assert(!gears::PublishRhiRendererFrameInput(2065, {}).has_value());
    const std::optional<gears::RhiRendererFrameComparison> expiredRendererSide =
        gears::ObserveRhiSemanticFrameSealed({.frameSequence = 2000});
    assert(expiredRendererSide.has_value());
    assert(expiredRendererSide->duplicate);

    for (std::uint64_t sequence = 3000; sequence < 3065; ++sequence)
    {
        assert(!gears::PublishRhiRendererFrameInput(sequence, {}).has_value());
        assert(gears::ObserveRhiSemanticFrameSealed({.frameSequence = sequence}).has_value());
    }
    const std::optional<gears::RhiRendererFrameComparison> evictedDuplicate =
        gears::PublishRhiRendererFrameInput(1000, {});
    assert(evictedDuplicate.has_value());
    assert(evictedDuplicate->duplicate);

    return 0;
}
