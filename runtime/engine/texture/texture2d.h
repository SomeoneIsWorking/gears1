#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "object/bulk_data.h"
#include "object/serialized_object.h"
#include "pixel_format.h"

namespace gears::engine::texture
{

struct TextureMip
{
    std::size_t width = 0;
    std::size_t height = 0;
    object::BulkData data;
};

// A Texture2D export: its format, its size, and its stored mips. Mip data
// stays as the console stores it until `LinearMip` converts one.
class Texture2D
{
  public:
    static Texture2D Read(const object::SerializedObject &object);

    [[nodiscard]] PixelFormat Format() const noexcept { return format_; }
    [[nodiscard]] std::size_t Width() const noexcept { return width_; }
    [[nodiscard]] std::size_t Height() const noexcept { return height_; }
    // Mips with stored data, largest first. A mip the texture lists with no
    // data (the console keeps small mips in a packed tail) is not included.
    [[nodiscard]] const std::vector<TextureMip> &Mips() const noexcept { return mips_; }

    // Mip `index` as linear host-order block rows, ready for a host texture
    // of the same block format. Separate-file mips are read through `files`.
    [[nodiscard]] std::vector<std::uint8_t> LinearMip(std::size_t index,
                                                      package::ContentFiles &files) const;

  private:
    Texture2D(PixelFormat format, std::size_t width, std::size_t height,
              std::vector<TextureMip> mips, std::string outer_package)
        : format_(format), width_(width), height_(height), mips_(std::move(mips)),
          outer_package_(std::move(outer_package))
    {
    }

    PixelFormat format_;
    std::size_t width_;
    std::size_t height_;
    std::vector<TextureMip> mips_;
    std::string outer_package_;
};

} // namespace gears::engine::texture
