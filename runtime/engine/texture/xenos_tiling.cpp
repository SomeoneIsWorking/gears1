#include "xenos_tiling.h"

#include <algorithm>
#include <bit>
#include <format>

#include "package/byte_reader.h"

namespace gears::engine::texture
{
namespace
{

std::size_t Align(std::size_t value, std::size_t alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

// Block index of block (x, y) in a tiled surface `pitch` blocks wide (a
// multiple of 32), for blocks of 2^log_bytes bytes.
std::size_t TiledBlockIndex(std::size_t x, std::size_t y, std::size_t pitch, std::size_t log_bytes)
{
    std::size_t macro = ((x >> 5U) + (y >> 5U) * (pitch >> 5U)) << (log_bytes + 7U);
    std::size_t micro = ((x & 7U) + ((y & 6U) << 2U)) << log_bytes;
    std::size_t offset = macro + ((micro & ~std::size_t{15}) << 1U) + (micro & 15U) +
                         ((y & 8U) << (3U + log_bytes)) + ((y & 1U) << 4U);
    std::size_t address = ((offset & ~std::size_t{511}) << 3U) + ((offset & 448U) << 2U) +
                          (offset & 63U) + ((y & 16U) << 7U) +
                          (((((y & 8U) >> 2U) + (x >> 3U)) & 3U) << 6U);
    return address >> log_bytes;
}

void SwapWords(std::span<std::uint8_t> block, std::size_t width)
{
    for (std::size_t i = 0; i + width <= block.size(); i += width)
    {
        std::reverse(block.begin() + static_cast<std::ptrdiff_t>(i),
                     block.begin() + static_cast<std::ptrdiff_t>(i + width));
    }
}

} // namespace

std::size_t TiledSurfaceBytes(std::size_t width, std::size_t height, const FormatLayout &layout)
{
    std::size_t blocks_wide = Align(Align(width, layout.block_edge) / layout.block_edge, 32U);
    std::size_t blocks_high = Align(Align(height, layout.block_edge) / layout.block_edge, 32U);
    return blocks_wide * blocks_high * layout.block_bytes;
}

std::vector<std::uint8_t> UntileXenos2D(std::span<const std::uint8_t> stored, std::size_t width,
                                        std::size_t height, const FormatLayout &layout)
{
    if (layout.block_bytes == 0U || !std::has_single_bit(layout.block_bytes))
    {
        throw package::PackageFormatError("untiling needs a power-of-two block size");
    }
    std::size_t required = TiledSurfaceBytes(width, height, layout);
    if (stored.size() < required)
    {
        throw package::PackageFormatError(
            std::format("tiled {}x{} surface needs {} byte(s) but {} are stored", width, height,
                        required, stored.size()));
    }
    std::size_t blocks_wide = Align(width, layout.block_edge) / layout.block_edge;
    std::size_t blocks_high = Align(height, layout.block_edge) / layout.block_edge;
    std::size_t pitch = Align(blocks_wide, 32U);
    auto log_bytes = static_cast<std::size_t>(std::countr_zero(layout.block_bytes));
    std::vector<std::uint8_t> linear(blocks_wide * blocks_high * layout.block_bytes);
    for (std::size_t y = 0; y < blocks_high; ++y)
    {
        for (std::size_t x = 0; x < blocks_wide; ++x)
        {
            std::size_t source = TiledBlockIndex(x, y, pitch, log_bytes) * layout.block_bytes;
            std::size_t target = (y * blocks_wide + x) * layout.block_bytes;
            std::copy_n(stored.begin() + static_cast<std::ptrdiff_t>(source), layout.block_bytes,
                        linear.begin() + static_cast<std::ptrdiff_t>(target));
            SwapWords(std::span(linear).subspan(target, layout.block_bytes), layout.swap_width);
        }
    }
    return linear;
}

} // namespace gears::engine::texture
