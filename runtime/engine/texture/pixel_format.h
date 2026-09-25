#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gears::engine::texture
{

// Pixel formats a Gears 1 texture's Format property names.
enum class PixelFormat : std::uint8_t
{
    Unknown = 0,
    A32B32G32R32F = 1,
    A8R8G8B8 = 2,
    G8 = 3,
    G16 = 4,
    Dxt1 = 5,
    Dxt3 = 6,
    Dxt5 = 7,
};

// Storage of one format: texels per block edge, bytes per block, and the
// width of the big-endian words the console stores them in.
struct FormatLayout
{
    std::size_t block_edge = 1;
    std::size_t block_bytes = 0;
    std::size_t swap_width = 1;
};

// Refuses (returns block_bytes 0 for) formats this engine does not decode.
constexpr FormatLayout LayoutOf(PixelFormat format) noexcept
{
    switch (format)
    {
    case PixelFormat::A8R8G8B8:
        return {1, 4, 4};
    case PixelFormat::G8:
        return {1, 1, 1};
    case PixelFormat::Dxt1:
        return {4, 8, 2};
    case PixelFormat::Dxt3:
    case PixelFormat::Dxt5:
        return {4, 16, 2};
    default:
        return {};
    }
}

constexpr std::string_view NameOf(PixelFormat format) noexcept
{
    switch (format)
    {
    case PixelFormat::A32B32G32R32F:
        return "A32B32G32R32F";
    case PixelFormat::A8R8G8B8:
        return "A8R8G8B8";
    case PixelFormat::G8:
        return "G8";
    case PixelFormat::G16:
        return "G16";
    case PixelFormat::Dxt1:
        return "DXT1";
    case PixelFormat::Dxt3:
        return "DXT3";
    case PixelFormat::Dxt5:
        return "DXT5";
    default:
        return "Unknown";
    }
}

} // namespace gears::engine::texture
