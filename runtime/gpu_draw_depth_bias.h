#pragma once

#include <cstdint>

namespace gears::draw
{

struct DepthBias
{
    float constantFactor = 0.0f;
    float slopeFactor = 0.0f;
};

// Selects the preferred non-culled face's guest polygon offset and converts
// Xenos subpixel slope and D24/float24 constant units to Vulkan units.
DepthBias DeriveDepthBias(const uint32_t* registerFile,
                          bool primitivePolygonal);

} // namespace gears::draw
