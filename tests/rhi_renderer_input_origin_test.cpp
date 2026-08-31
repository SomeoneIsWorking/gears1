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
