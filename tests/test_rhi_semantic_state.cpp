#include "rhi_semantic_state.h"

#include <cassert>

int main()
{
    gears::RhiSemanticStateTracker tracker;
    tracker.ApplyBinding({
        .kind = gears::RhiSemanticBindingKind::VertexStream,
        .slot = 3,
        .object = 0x40102000,
        .bufferViewPresent = true,
        .bufferView =
            {
                .allocation = {.guestAddress = 0x00102040, .sizeBytes = 0x2FC0},
                .elementStrideBytes = 20,
            },
    });
    tracker.ApplyBinding({
        .kind = gears::RhiSemanticBindingKind::DepthStencilTarget,
        .object = 0x40104000,
        .descriptor = {0x2D0, 0x20},
        .descriptorDwords = 2,
    });
    tracker.ApplyBinding(
        {
            .kind = gears::RhiSemanticBindingKind::ColorRenderTarget,
            .slot = 0,
            .object = 0x40103000,
            .descriptor = {0x000302D0},
            .descriptorDwords = 1,
        },
        {
            .present = true,
            .observedObject = 0x40103000,
            .descriptor = {0x000302D0},
            .descriptorDwords = 1,
            .targetStatePresent = true,
            .targetState = {.base = 0x2D0, .format = 3},
            .surfaceStatePresent = true,
            .surfaceState = {.pitch = 1280, .msaaSamples = 0},
        });
    tracker.ApplyBinding(
        {
            .kind = gears::RhiSemanticBindingKind::Texture,
            .slot = 3,
            .object = 0x40103000,
        },
        {
            .present = true,
            .observedObject = 0x40103000,
            .descriptor = {1, 2, 3, 4, 5, 6},
            .descriptorDwords = 6,
        });
    tracker.ApplyBinding({
        .kind = gears::RhiSemanticBindingKind::PixelShader,
        .object = 0x40106000,
    });
    gears::RhiBindingStateEvidence patchedTextureState{
        .present = true,
        .observedObject = 0x40106000,
        .textureFetchStatePresent = true,
        .shaderModulesPresent = true,
        .shaderModules = {{.guestAddress = 0x00110000,
                           .sizeBytes = 48,
                           .hash = 0x123456789ABCDEF0ull}},
    };
    patchedTextureState.textureFetchState[3] = {6, 5, 4, 3, 2, 1};
    tracker.ApplyBinding({.kind = gears::RhiSemanticBindingKind::PixelShader, .object = 0x40106000},
                         patchedTextureState);
    tracker.ApplyBinding({.kind = gears::RhiSemanticBindingKind::TextureState,
                          .slot = 3,
                          .object = 0x40103000,
                          .descriptor = {6, 5, 4, 0x00280C14, 2, 1},
                          .descriptorDwords = 6},
                         {.present = true,
                          .observedObject = 0x40103000,
                          .descriptor = {6, 5, 4, 0x00280C14, 2, 1},
                          .descriptorDwords = 6});
    tracker.ApplyBinding({
        .kind = gears::RhiSemanticBindingKind::VertexShader,
        .object = 0x40107000,
        .shaderModules = {{.guestAddress = 0x00120000,
                           .sizeBytes = 60,
                           .hash = 0x1122334455667788ull}},
    });
    tracker.ApplyBinding({
        .kind = gears::RhiSemanticBindingKind::IndexBuffer,
        .object = 0x40108000,
        .bufferViewPresent = true,
        .bufferView =
            {
                .allocation = {.guestAddress = 0x00108000, .sizeBytes = 0x2000},
                .elementStrideBytes = 2,
                .endianSwap = 1,
            },
    });
    tracker.ApplyBinding({
        .kind = gears::RhiSemanticBindingKind::VertexStream,
        .slot = 1,
        .object = 0x40101000,
        .bufferViewPresent = true,
        .bufferView =
            {
                .allocation = {.guestAddress = 0x00100000, .sizeBytes = 0x1000},
                .elementStrideBytes = 12,
            },
    });

    const gears::RhiSemanticDraw draw{
        .kind = gears::RhiSemanticDrawKind::BoundIndices,
        .primitiveType = 4,
        .elementCount = 36,
    };
    gears::RhiSemanticDrawState state = tracker.SnapshotDraw(draw);
    assert(state.draw.elementCount == 36);
    assert(state.vertexStreams.size() == 2);
    assert(state.vertexStreams[0].slot == 1);
    assert(state.vertexStreams[0].object == 0x40101000);
    assert(state.vertexStreams[1].slot == 3);
    assert(state.vertexStreams[1].object == 0x40102000);
    assert(state.vertexStreams[1].view.allocation.guestAddress == 0x00102040);
    assert(state.vertexStreams[1].view.elementStrideBytes == 20);
    assert(state.renderTargets.size() == 2);
    assert(!state.renderTargets[0].depthStencil);
    assert(state.renderTargets[0].slot == 0);
    assert(state.renderTargets[0].object == 0x40103000);
    assert(state.renderTargets[1].depthStencil);
    assert(state.renderTargets[1].object == 0x40104000);
    assert(state.renderTargets[0].descriptorDwords == 1);
    assert(state.renderTargets[0].descriptor[0] == 0x000302D0);
    assert(state.renderTargets[0].normalizedStatePresent);
    assert(state.renderTargets[0].normalizedState.base == 0x2D0);
    assert(state.renderTargets[0].normalizedState.format == 3);
    assert(state.surfaceStatePresent);
    assert(state.surfaceState.pitch == 1280);
    assert(state.textures.size() == 1);
    assert(state.textures[0].slot == 3);
    assert(state.textures[0].descriptorDwords == 6);
    assert(state.textures[0].descriptor[0] == 6);
    assert(state.textures[0].descriptor[3] == 0x00280C14);
    assert(state.textures[0].descriptor[5] == 1);
    assert(state.pixelShader.has_value());
    assert(state.pixelShader->object == 0x40106000);
    assert(state.lastPixelShaderBinding.has_value());
    assert(state.lastPixelShaderBinding->object == 0x40106000);
    assert(state.pixelShader->shaderModules.size() == 1);
    assert(state.pixelShader->shaderModules[0].hash == 0x123456789ABCDEF0ull);
    assert(state.vertexShader.has_value());
    assert(state.vertexShader->object == 0x40107000);
    assert(state.vertexShader->shaderModules.size() == 1);

    tracker.ApplyBinding({.kind = gears::RhiSemanticBindingKind::PixelShader, .object = 0x40106000},
                         {.present = true, .observedObject = 0x40106000});
    state = tracker.SnapshotDraw(draw);
    assert(state.pixelShader.has_value());
    assert(state.pixelShader->shaderModules.size() == 1);
    tracker.ApplyBinding({.kind = gears::RhiSemanticBindingKind::PixelShader, .object = 0x40106100},
                         {.present = true, .observedObject = 0x40106100});
    state = tracker.SnapshotDraw(draw);
    assert(state.pixelShader.has_value());
    assert(state.pixelShader->shaderModules.empty());
    tracker.ApplyBinding(
        {.kind = gears::RhiSemanticBindingKind::PixelShader, .object = 0x40106100},
        {.present = true,
         .observedObject = 0x40106100,
         .shaderModulesPresent = true,
         .shaderModules = {
             {.guestAddress = 0x00111000, .sizeBytes = 48, .hash = 0x123456789ABCDEF0ull}}});
    assert(state.indexBuffer.has_value());
    assert(state.indexBuffer->bufferView.elementStrideBytes == 2);

    const gears::RhiSemanticColorWriteState gammaWrite{
        .requested = 1,
        .targetPresent = true,
        .target = {.slot = 0,
                   .object = 0x40103000,
                   .descriptor = {0x000C02D0},
                   .descriptorDwords = 1,
                   .normalizedStatePresent = true,
                   .normalizedState = {.base = 0x2D0, .format = 12}},
        .surfaceStatePresent = true,
        .surfaceState = {.pitch = 1280, .msaaSamples = 0},
    };
    assert(tracker.ApplyColorWriteState(gammaWrite) ==
           gears::RhiColorWriteStateEvidenceResult::Match);
    state = tracker.SnapshotDraw(draw);
    assert(state.renderTargets[0].normalizedState.format == 12);
    auto wrongTarget = gammaWrite;
    wrongTarget.target.object ^= 4;
    assert(tracker.ApplyColorWriteState(wrongTarget) ==
           gears::RhiColorWriteStateEvidenceResult::Mismatch);
    assert(tracker.SnapshotDraw(draw).renderTargets[0].normalizedState.format == 12);

    tracker.ApplyBinding({
        .kind = gears::RhiSemanticBindingKind::VertexStream,
        .slot = 3,
    });
    state = tracker.SnapshotDraw(draw);
    assert(state.vertexStreams.size() == 1);
    assert(state.vertexStreams[0].slot == 1);

    tracker.ApplyBinding({
        .kind = gears::RhiSemanticBindingKind::VertexStream,
        .slot = 4,
        .object = 0x40105000,
        .bufferViewPresent = true,
        .bufferView = {.allocation = {.guestAddress = 0x00105000, .sizeBytes = 0x800}},
    });
    tracker.ApplyVertexStreamReset({.firstSlot = 0, .slotCount = 4});
    state = tracker.SnapshotDraw(draw);
    assert(state.vertexStreams.size() == 1);
    assert(state.vertexStreams[0].slot == 4);

    tracker.ApplyBinding({
        .kind = gears::RhiSemanticBindingKind::ColorRenderTarget,
        .slot = 0,
    });
    tracker.ApplyBinding({
        .kind = gears::RhiSemanticBindingKind::DepthStencilTarget,
    });
    state = tracker.SnapshotDraw(draw);
    assert(state.renderTargets.empty());

    tracker.ApplyBinding({
        .kind = gears::RhiSemanticBindingKind::Texture,
        .slot = 3,
    });
    tracker.ApplyBinding({.kind = gears::RhiSemanticBindingKind::PixelShader,
                          .origin = gears::RhiSemanticBindingOrigin::Setter});
    tracker.ApplyBinding({.kind = gears::RhiSemanticBindingKind::VertexShader});
    tracker.ApplyBinding({.kind = gears::RhiSemanticBindingKind::IndexBuffer});
    state = tracker.SnapshotDraw(draw);
    assert(state.textures.empty());
    assert(!state.pixelShader.has_value());
    assert(state.lastPixelShaderBinding.has_value());
    assert(state.lastPixelShaderBinding->origin == gears::RhiSemanticBindingOrigin::Setter);
    assert(state.lastPixelShaderBinding->object == 0);
    assert(!state.vertexShader.has_value());
    assert(!state.indexBuffer.has_value());

    tracker.ApplyBinding(
        {.kind = gears::RhiSemanticBindingKind::PixelShader,
         .origin = gears::RhiSemanticBindingOrigin::Flush},
        {.present = true,
         .observedObject = 0,
         .shaderModulesPresent = true,
         .shaderModules = {{.guestAddress = 0, .sizeBytes = 48, .hash = 0xCAFEBABE12345678ull}}});
    state = tracker.SnapshotDraw(draw);
    assert(state.pixelShader.has_value());
    assert(state.pixelShader->object == 0);
    assert(state.pixelShader->shaderModules.size() == 1);
    assert(state.pixelShader->shaderModules.front().hash == 0xCAFEBABE12345678ull);

    tracker.ApplyBinding({.kind = gears::RhiSemanticBindingKind::PixelShader,
                          .origin = gears::RhiSemanticBindingOrigin::Setter});
    assert(!tracker.SnapshotDraw(draw).pixelShader.has_value());

    tracker.Reset();
    assert(tracker.SnapshotDraw(draw).vertexStreams.empty());
    assert(!tracker.SnapshotDraw(draw).lastPixelShaderBinding.has_value());
    assert(tracker.ApplyColorWriteState(gammaWrite) ==
           gears::RhiColorWriteStateEvidenceResult::Missing);
    assert(tracker.ApplyColorWriteState({.requested = 1}) ==
           gears::RhiColorWriteStateEvidenceResult::Match);

    return 0;
}
