#pragma once

#include <cstdint>

namespace gears
{

// Host-API-independent target state shared by the title semantic adapter and
// the compatibility-front-end decoder. Native consumers receive these values;
// packed device shadows and Xenos register files stop at their producers.
struct RhiRenderTargetDescriptorState
{
    std::uint32_t base = 0;
    std::uint32_t format = 0;
    std::int32_t colorExponentBias = 0;

    bool operator==(const RhiRenderTargetDescriptorState &) const = default;
};

struct RhiSurfaceState
{
    std::uint32_t pitch = 0;
    std::uint32_t msaaSamples = 0;

    bool operator==(const RhiSurfaceState &) const = default;
};

[[nodiscard]] constexpr std::int32_t DecodeRhiSignedColorExponent(std::uint32_t descriptor)
{
    std::int32_t exponent = static_cast<std::int32_t>((descriptor >> 20) & 0x3F);
    if ((exponent & 0x20) != 0)
        exponent -= 64;
    return exponent;
}

[[nodiscard]] constexpr RhiRenderTargetDescriptorState
DecodeRhiColorTargetDescriptor(std::uint32_t descriptor)
{
    return {
        .base = descriptor & 0xFFF,
        .format = (descriptor >> 16) & 0xF,
        .colorExponentBias = DecodeRhiSignedColorExponent(descriptor),
    };
}

[[nodiscard]] constexpr RhiRenderTargetDescriptorState
DecodeRhiDepthTargetDescriptor(std::uint32_t descriptor)
{
    return {
        .base = descriptor & 0xFFF,
        .format = (descriptor >> 16) & 1,
    };
}

[[nodiscard]] constexpr RhiSurfaceState DecodeRhiSurfaceState(std::uint32_t surfaceInfo)
{
    return {
        .pitch = surfaceInfo & 0x3FFF,
        .msaaSamples = (surfaceInfo >> 16) & 3,
    };
}

} // namespace gears
