#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "pixel_format.h"

namespace gears::engine::texture
{

// The storage extent of a tiled 2D surface: whole 32x32-block macro tiles in
// both directions.
[[nodiscard]] std::size_t TiledSurfaceBytes(std::size_t width, std::size_t height,
                                            const FormatLayout &layout);

// Converts one mip as the console stores it (2D-tiled blocks in big-endian
// words) into linear, host-order rows of blocks. `stored` must hold at least
// `TiledSurfaceBytes` bytes. Independently written from the published Xenos
// tiled-address layout.
[[nodiscard]] std::vector<std::uint8_t> UntileXenos2D(std::span<const std::uint8_t> stored,
                                                      std::size_t width, std::size_t height,
                                                      const FormatLayout &layout);

} // namespace gears::engine::texture
