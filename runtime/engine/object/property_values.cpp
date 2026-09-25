#include "property_values.h"

#include <bit>
#include <format>

#include "package/byte_reader.h"

namespace gears::engine::object
{
namespace
{

std::uint32_t BigEndian32(std::span<const std::uint8_t> bytes)
{
    return (std::uint32_t{bytes[0]} << 24U) | (std::uint32_t{bytes[1]} << 16U) |
           (std::uint32_t{bytes[2]} << 8U) | std::uint32_t{bytes[3]};
}

} // namespace

const PropertyTag *PropertyValues::Tag(std::string_view name, std::string_view type,
                                       std::size_t size) const
{
    const PropertyTag *tag = object_.Find(name);
    if (tag == nullptr)
    {
        return nullptr;
    }
    std::string stored_type = object_.Owner().NameText(tag->type);
    if (stored_type != type || (size != 0U && tag->value.size() != size))
    {
        throw package::PackageFormatError(
            std::format("property '{}' is a {}-byte {}, not a {}-byte {}", name, tag->value.size(),
                        stored_type, size, type));
    }
    return tag;
}

std::int32_t PropertyValues::Int(std::string_view name, std::int32_t fallback) const
{
    const PropertyTag *tag = Tag(name, "IntProperty", 4U);
    return tag == nullptr ? fallback : static_cast<std::int32_t>(BigEndian32(tag->value));
}

float PropertyValues::Float(std::string_view name, float fallback) const
{
    const PropertyTag *tag = Tag(name, "FloatProperty", 4U);
    return tag == nullptr ? fallback : std::bit_cast<float>(BigEndian32(tag->value));
}

std::uint8_t PropertyValues::Byte(std::string_view name, std::uint8_t fallback) const
{
    const PropertyTag *tag = Tag(name, "ByteProperty", 1U);
    return tag == nullptr ? fallback : tag->value[0];
}

bool PropertyValues::Bool(std::string_view name, bool fallback) const
{
    const PropertyTag *tag = Tag(name, "BoolProperty", 0U);
    return tag == nullptr ? fallback : tag->bool_value;
}

PackageIndex PropertyValues::Object(std::string_view name) const
{
    const PropertyTag *tag = Tag(name, "ObjectProperty", 4U);
    return tag == nullptr ? 0 : static_cast<PackageIndex>(BigEndian32(tag->value));
}

std::span<const std::uint8_t>
PropertyValues::Struct(std::string_view name, std::string_view struct_name, std::size_t size) const
{
    const PropertyTag *tag = Tag(name, "StructProperty", size);
    if (tag == nullptr)
    {
        return {};
    }
    std::string stored = object_.Owner().NameText(tag->struct_name);
    if (stored != struct_name)
    {
        throw package::PackageFormatError(
            std::format("property '{}' is a {} struct, not a {}", name, stored, struct_name));
    }
    return tag->value;
}

std::span<const std::uint8_t> PropertyValues::Array(std::string_view name,
                                                    std::size_t element_size) const
{
    const PropertyTag *tag = Tag(name, "ArrayProperty", 0U);
    if (tag == nullptr)
    {
        return {};
    }
    if (tag->value.size() < 4U)
    {
        throw package::PackageFormatError(std::format("array '{}' has no count", name));
    }
    std::size_t count = BigEndian32(tag->value);
    if (tag->value.size() != 4U + count * element_size)
    {
        throw package::PackageFormatError(
            std::format("array '{}' of {} element(s) holds {} byte(s), not {} per element", name,
                        count, tag->value.size() - 4U, element_size));
    }
    return tag->value.subspan(4U);
}

} // namespace gears::engine::object
