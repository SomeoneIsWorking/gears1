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
            .slot = 2,
            .object = 0x40103000,
            .descriptor = {0x97813},
            .descriptorDwords = 1,
        },
        {
            .present = true,
            .observedObject = 0x40103000,
            .descriptor = {0xabcde},
            .descriptorDwords = 1,
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
    tracker.ApplyBinding({
        .kind = gears::RhiSemanticBindingKind::VertexShader,
        .object = 0x40107000,
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
    assert(state.renderTargets[0].slot == 2);
    assert(state.renderTargets[0].object == 0x40103000);
    assert(state.renderTargets[1].depthStencil);
    assert(state.renderTargets[1].object == 0x40104000);
    assert(state.renderTargets[0].descriptorDwords == 1);
    assert(state.renderTargets[0].descriptor[0] == 0xabcde);
    assert(state.textures.size() == 1);
    assert(state.textures[0].slot == 3);
    assert(state.textures[0].descriptorDwords == 6);
    assert(state.textures[0].descriptor[5] == 6);
    assert(state.pixelShader.has_value());
    assert(state.pixelShader->object == 0x40106000);
    assert(state.vertexShader.has_value());
    assert(state.vertexShader->object == 0x40107000);
    assert(state.indexBuffer.has_value());
    assert(state.indexBuffer->bufferView.elementStrideBytes == 2);

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
        .slot = 2,
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
    tracker.ApplyBinding({.kind = gears::RhiSemanticBindingKind::PixelShader});
    tracker.ApplyBinding({.kind = gears::RhiSemanticBindingKind::VertexShader});
    tracker.ApplyBinding({.kind = gears::RhiSemanticBindingKind::IndexBuffer});
    state = tracker.SnapshotDraw(draw);
    assert(state.textures.empty());
    assert(!state.pixelShader.has_value());
    assert(!state.vertexShader.has_value());
    assert(!state.indexBuffer.has_value());

    tracker.Reset();
    assert(tracker.SnapshotDraw(draw).vertexStreams.empty());

    return 0;
}
