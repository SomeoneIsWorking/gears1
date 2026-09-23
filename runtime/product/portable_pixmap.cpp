#include "portable_pixmap.h"

#include <cstddef>
#include <cstdint>
#include <format>

namespace gears::product
{
namespace
{

constexpr std::size_t kPixmapChannels = 3;
constexpr std::size_t kGuestPixelBytes = 4;

} // namespace

std::string EncodePortablePixmap(const x360port::SystemFrameImage &image)
{
    std::string pixmap = std::format("P6\n{} {}\n255\n", image.width, image.height);
    std::size_t header_size = pixmap.size();
    std::size_t row_size = static_cast<std::size_t>(image.width) * kPixmapChannels;
    pixmap.resize(header_size + row_size * image.height);
    char *destination = pixmap.data() + header_size;
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        const std::byte *source = image.pixels.data() + static_cast<std::size_t>(y) * image.stride;
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            for (std::size_t channel = 0; channel < kPixmapChannels; ++channel)
            {
                *destination++ = static_cast<char>(source[x * kGuestPixelBytes + channel]);
            }
        }
    }
    return pixmap;
}

} // namespace gears::product
