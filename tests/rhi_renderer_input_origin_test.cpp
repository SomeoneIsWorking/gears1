#include "rhi_renderer_input_origin_test.h"

#include "rhi_renderer_input.h"
#include "rhi_semantic_stream.h"

#include <cassert>
#include <cstddef>

void TestRhiRendererPixelBindingOriginCensus()
{
    const gears::RhiSemanticFrame frame{
        .events =
            {
                {.payload =
                     gears::RhiObservedBinding{
                         .binding = {.kind = gears::RhiSemanticBindingKind::PixelShader,
                                     .origin = gears::RhiSemanticBindingOrigin::Setter,
                                     .object = 0x40102000},
                         .state = {.present = true, .observedObject = 0},
                     }},
                {.payload =
                     gears::RhiObservedBinding{
                         .binding = {.kind = gears::RhiSemanticBindingKind::PixelShader,
                                     .origin = gears::RhiSemanticBindingOrigin::Flush,
                                     .object = 0x40103000},
                         .state = {.present = true, .observedObject = 0x40103000},
                     }},
            },
    };
    const gears::RhiRendererFrameComparison comparison = gears::CompareRhiRendererDraws(frame, {});
    assert(comparison.pixelShaderBindingsByOrigin[static_cast<std::size_t>(
               gears::RhiSemanticBindingOrigin::Setter)] == 1);
    assert(comparison.pixelShaderBindingsByOrigin[static_cast<std::size_t>(
               gears::RhiSemanticBindingOrigin::Flush)] == 1);
    assert(comparison.pixelShaderClearsByOrigin[static_cast<std::size_t>(
               gears::RhiSemanticBindingOrigin::Setter)] == 1);
    assert(comparison.pixelShaderClearsByOrigin[static_cast<std::size_t>(
               gears::RhiSemanticBindingOrigin::Flush)] == 0);
}

void TestRhiRendererViewportParity()
{
    const gears::RhiViewportState viewport{.x = 4,
                                           .y = 8,
                                           .w = 640,
                                           .h = 360,
                                           .zMin = 0.25f,
                                           .zMax = 0.75f,
                                           .scissorX = 2,
                                           .scissorY = 3,
                                           .scissorW = 636,
                                           .scissorH = 354};
    const gears::RhiSemanticDrawState semantic{
        .draw = {.kind = gears::RhiSemanticDrawKind::BoundVertices,
                 .primitiveType = 4,
                 .elementCount = 3},
        .viewportStatePresent = true,
        .viewportState = viewport,
    };
    gears::RhiRendererDrawInput renderer{
        .outcome = gears::draw::NativeDrawMaterializationOutcome::Materialized,
        .primitiveType = 4,
        .elementCount = 3,
        .viewportStatePresent = true,
        .viewportState = viewport,
    };
    assert(gears::CompareRhiRendererDrawInput(semantic, renderer) ==
           gears::RhiRendererDrawEvidenceResult::Match);

    renderer.viewportState.scissorH += 1;
    const gears::RhiRendererDrawEvidence mismatch =
        gears::InspectRhiRendererDrawInput(semantic, renderer);
    assert(mismatch.result == gears::RhiRendererDrawEvidenceResult::Mismatch);
    assert(mismatch.reason == gears::RhiRendererDrawEvidenceReason::ViewportState);

    renderer.viewportState = viewport;
    renderer.viewportStatePresent = false;
    assert(gears::InspectRhiRendererDrawInput(semantic, renderer).reason ==
           gears::RhiRendererDrawEvidenceReason::RendererViewportStateMissing);

    auto missingSemantic = semantic;
    missingSemantic.viewportStatePresent = false;
    renderer.viewportStatePresent = true;
    assert(gears::InspectRhiRendererDrawInput(missingSemantic, renderer).reason ==
           gears::RhiRendererDrawEvidenceReason::SemanticViewportStateMissing);
}
