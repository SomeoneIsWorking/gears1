#pragma once

#include <cstdint>

namespace gears
{

constexpr uint16_t SwapGpuIndex16(uint16_t value, uint32_t endian)
{
    // For 16-bit DMA, Xenia maps k8in32 to k8in16 and k16in32 to kNone.
    return endian == 1 || endian == 2 ? uint16_t((value >> 8) | (value << 8)) : value;
}

constexpr uint32_t SwapGpuWord32(uint32_t value, uint32_t endian)
{
    switch (endian & 3u)
    {
    case 0:
        return value;
    case 1:
        return ((value & 0x00FF00FFu) << 8) | ((value & 0xFF00FF00u) >> 8);
    case 2:
        return __builtin_bswap32(value);
    case 3:
        return (value << 16) | (value >> 16);
    }
    return value;
}

constexpr uint32_t LoadGpuWord32(const uint8_t *bytes, uint32_t endian)
{
    const uint32_t value = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
                           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
    return SwapGpuWord32(value, endian);
}

} // namespace gears
