#pragma once

#include <cstdint>

namespace gears::draw
{

struct RenderExtentCapacity
{
    uint32_t width;
    uint32_t height;
};

// Persistent EDRAM images are capacity allocations. A frame using fewer
// samples fits in a larger image and must not tear the cache down merely
// because its active extent is smaller.
constexpr bool RenderExtentNeedsGrowth(uint32_t capacityWidth,
                                       uint32_t capacityHeight,
                                       uint32_t requiredWidth,
                                       uint32_t requiredHeight)
{
    return requiredWidth > capacityWidth || requiredHeight > capacityHeight;
}

constexpr RenderExtentCapacity GrowRenderExtentCapacity(
    uint32_t capacityWidth, uint32_t capacityHeight,
    uint32_t requiredWidth, uint32_t requiredHeight)
{
    return {
        requiredWidth > capacityWidth ? requiredWidth : capacityWidth,
        requiredHeight > capacityHeight ? requiredHeight : capacityHeight,
    };
}

} // namespace gears::draw
