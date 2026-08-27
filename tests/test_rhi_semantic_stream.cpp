#include "rhi_semantic_stream.h"

#include <cassert>
#include <cstdlib>
#include <variant>

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
    using gears::RhiSemanticBinding;
    using gears::RhiSemanticBindingKind;
    using gears::RhiSemanticDraw;
    using gears::RhiSemanticDrawKind;
    using gears::RhiSemanticPresent;

    const RhiSemanticDraw autoIndexed{
        .kind = RhiSemanticDrawKind::BoundVertices,
        .primitiveType = 4,
        .elementCount = 300,
    };
    const RhiDrawPacketEvidence matchingAuto{
        .present = true,
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

    assert(setenv("GEARS_NATIVE_RHI_OBSERVE", "1", 1) == 0);
    lucent::config::set_prefix("GEARS_");
    gears::ObserveRhiSemanticDraw(autoIndexed, matchingAuto);
    gears::ObserveRhiSemanticBinding(textureBinding, matchingBinding);
    altered = matchingIndexed;
    altered.sourceSelect = 2;
    gears::ObserveRhiSemanticDraw(indexed, altered);
    gears::ObserveRhiSemanticPresent(present, matchingPresent);
    const gears::RhiSemanticFrame frame = gears::SealRhiSemanticFrame(17);
    assert(frame.frameSequence == 17);
    assert(frame.events.size() == 4);
    assert(frame.draws == 2);
    assert(frame.bindings == 1);
    assert(frame.presents == 1);
    assert(frame.events[0].sequence + 1 == frame.events[1].sequence);
    assert(frame.events[1].sequence + 1 == frame.events[2].sequence);
    assert(frame.events[2].sequence + 1 == frame.events[3].sequence);
    assert(std::holds_alternative<gears::RhiObservedDraw>(frame.events[0].payload));
    assert(std::holds_alternative<gears::RhiObservedBinding>(frame.events[1].payload));
    assert(std::holds_alternative<gears::RhiObservedDraw>(frame.events[2].payload));
    assert(std::holds_alternative<gears::RhiObservedPresent>(frame.events[3].payload));
    assert(std::get<gears::RhiObservedDraw>(frame.events[0].payload).state.draw.elementCount ==
           300);
    assert(std::get<gears::RhiObservedBinding>(frame.events[1].payload).binding.slot == 3);
    assert(std::get<gears::RhiObservedDraw>(frame.events[2].payload).state.draw.elementCount == 42);
    assert(std::get<gears::RhiObservedPresent>(frame.events[3].payload).present.frontBuffer ==
           0xA0311000);
    assert(frame.matched == 1);
    assert(frame.missing == 0);
    assert(frame.mismatched == 1);
    assert(frame.bindingsMatched == 1);
    assert(frame.bindingsMissing == 0);
    assert(frame.bindingsMismatched == 0);
    assert(frame.presentsMatched == 1);
    assert(frame.presentsMissing == 0);
    assert(frame.presentsMismatched == 0);

    return 0;
}
