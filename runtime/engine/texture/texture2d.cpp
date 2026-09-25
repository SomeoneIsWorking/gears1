#include "texture2d.h"

#include <format>

#include "object/property_values.h"
#include "package/byte_reader.h"
#include "xenos_tiling.h"

namespace gears::engine::texture
{

Texture2D Texture2D::Read(const object::SerializedObject &object)
{
    object::PropertyValues properties(object.Properties());
    auto format = static_cast<PixelFormat>(properties.Byte("Format"));
    std::int32_t width = properties.Int("SizeX");
    std::int32_t height = properties.Int("SizeY");
    if (width <= 0 || height <= 0)
    {
        throw package::PackageFormatError(
            std::format("texture declares size {}x{}", width, height));
    }
    package::ByteReader reader(object.NativeData(), package::ByteOrder::Big);
    // The uncooked source art; cooked textures store it empty.
    (void)object::BulkData::Read(reader, object.NativeBase());
    std::size_t mip_count = reader.ReadCount(24U);
    std::vector<TextureMip> mips;
    mips.reserve(mip_count);
    for (std::size_t i = 0; i < mip_count; ++i)
    {
        object::BulkData data = object::BulkData::Read(reader, object.NativeBase());
        std::int32_t mip_width = reader.ReadI32();
        std::int32_t mip_height = reader.ReadI32();
        if (mip_width <= 0 || mip_height <= 0)
        {
            reader.Fail(std::format("mip {} declares size {}x{}", i, mip_width, mip_height));
        }
        if (data.ElementCount() != 0U)
        {
            mips.push_back(
                {static_cast<std::size_t>(mip_width), static_cast<std::size_t>(mip_height), data});
        }
    }
    if (reader.Remaining() != 0U)
    {
        reader.Fail(std::format("{} byte(s) follow the texture's mips", reader.Remaining()));
    }
    return {format, static_cast<std::size_t>(width), static_cast<std::size_t>(height),
            std::move(mips), object.Owner().OutermostName(object.Index())};
}

std::vector<std::uint8_t> Texture2D::LinearMip(std::size_t index,
                                               package::ContentFiles &files) const
{
    const TextureMip &mip = mips_.at(index);
    FormatLayout layout = LayoutOf(format_);
    if (layout.block_bytes == 0U)
    {
        throw package::PackageFormatError(
            std::format("texture format {} is not decoded", NameOf(format_)));
    }
    return UntileXenos2D(mip.data.Decode(files, outer_package_), mip.width, mip.height, layout);
}

} // namespace gears::engine::texture
