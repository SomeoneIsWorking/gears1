#include "tagged_properties.h"

#include <format>
#include <string>

#include "package/package_constants.h"

namespace gears::engine::object
{

using package::ByteOrder;
using package::ByteReader;
using package::ReadNameReference;

TaggedProperties TaggedProperties::Read(ByteReader &reader, const Package &package)
{
    TaggedProperties properties(package);
    while (true)
    {
        PropertyTag tag;
        tag.name = ReadNameReference(reader);
        std::string name = package.NameText(tag.name);
        if (name == "None")
        {
            break;
        }
        tag.type = ReadNameReference(reader);
        std::string type = package.NameText(tag.type);
        std::size_t size = reader.ReadCount(1);
        tag.array_index = reader.ReadI32();
        if (type == "StructProperty")
        {
            tag.struct_name = ReadNameReference(reader);
            if (!package.HasName(tag.struct_name))
            {
                reader.Fail(std::format("struct '{}' names entry {} of {}", name,
                                        tag.struct_name.index, package.Tables().names.size()));
            }
        }
        else if (type == "BoolProperty")
        {
            std::uint32_t value = reader.ReadU32();
            if (value > 1U || size != 0U)
            {
                reader.Fail(
                    std::format("boolean '{}' has value {} and size {}", name, value, size));
            }
            tag.bool_value = value != 0U;
        }
        tag.value = reader.ReadBytes(size);
        properties.tags_.push_back(tag);
    }
    return properties;
}

TaggedProperties TaggedProperties::Parse(std::span<const std::uint8_t> bytes,
                                         const Package &package)
{
    ByteReader reader(bytes, ByteOrder::Big);
    TaggedProperties properties = Read(reader, package);
    if (reader.Remaining() != 0U)
    {
        reader.Fail(std::format("{} byte(s) follow a tagged struct's None", reader.Remaining()));
    }
    return properties;
}

std::vector<TaggedProperties> TaggedProperties::ParseArray(std::span<const std::uint8_t> bytes,
                                                           const Package &package)
{
    ByteReader reader(bytes, ByteOrder::Big);
    // Each element holds at least its terminating name reference.
    std::size_t count = reader.ReadCount(package::kNameReferenceSize);
    std::vector<TaggedProperties> elements;
    elements.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        elements.push_back(Read(reader, package));
    }
    if (reader.Remaining() != 0U)
    {
        reader.Fail(std::format("{} byte(s) follow the last of {} tagged struct element(s)",
                                reader.Remaining(), count));
    }
    return elements;
}

const PropertyTag *TaggedProperties::Find(std::string_view name, std::int32_t array_index) const
{
    for (const PropertyTag &tag : tags_)
    {
        if (tag.array_index == array_index && package_->NameText(tag.name) == name)
        {
            return &tag;
        }
    }
    return nullptr;
}

} // namespace gears::engine::object
