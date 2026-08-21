#pragma once

// Host attachment geometry for one Xenos RB_SURFACE_INFO.msaa_samples value.
//
// The legacy representation is a one-sample image in EDRAM sample coordinates.
// It remains the canonical 1X/4X view because those counts alias each other in
// this title. 2X uses a real two-sample attachment: Vulkan's standard 2X sample
// locations are the diagonal locations Xenos uses, while a vertically expanded
// one-sample image necessarily loses their horizontal component (catalog #115).

#include <cstdint>

namespace gears::draw
{

struct DrawSampleLayout
{
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    uint32_t rasterSamples = 1;
    uint32_t viewportScaleX = 1;
    uint32_t viewportScaleY = 1;

    constexpr bool IsNativeMultisample() const { return rasterSamples > 1; }
};

constexpr DrawSampleLayout DeriveDrawSampleLayout(uint32_t msaaSamples, uint32_t sampleGridWidth,
                                                  uint32_t sampleGridHeight)
{
    if (msaaSamples == 1) // Xenos k2X
        return {sampleGridWidth, sampleGridHeight / 2, 2, 1, 1};
    if (msaaSamples >= 2) // k4X remains in the canonical expanded sample grid.
        return {sampleGridWidth, sampleGridHeight, 1, 2, 2};
    return {sampleGridWidth, sampleGridHeight, 1, 1, 1};
}

} // namespace gears::draw
