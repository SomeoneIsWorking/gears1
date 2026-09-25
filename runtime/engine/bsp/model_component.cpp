#include "model_component.h"

#include <format>

#include "package/byte_reader.h"

namespace gears::engine::bsp
{
namespace
{

using package::ByteOrder;
using package::ByteReader;

// Light map kinds and stored sizes measured on the retail disc.
constexpr std::uint32_t kLightMapNone = 0;
constexpr std::uint32_t kLightMap2D = 2;
constexpr std::size_t kGuidSize = 16;
// Three coefficient textures, each with an RGB scale, then the coordinate
// scale and bias.
constexpr std::size_t kLightMap2DSize = 3U * 16U + 16U;

void SkipLightMap(ByteReader &reader)
{
    std::uint32_t kind = reader.ReadU32();
    if (kind == kLightMapNone)
    {
        return;
    }
    if (kind != kLightMap2D)
    {
        reader.Fail(std::format("light map kind {} is not measured", kind));
    }
    std::size_t lights = reader.ReadCount(kGuidSize);
    (void)reader.ReadBytes(lights * kGuidSize + kLightMap2DSize);
}

std::vector<std::uint16_t> ReadNodeList(ByteReader &reader)
{
    std::size_t count = reader.ReadCount(2);
    std::vector<std::uint16_t> nodes;
    nodes.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        nodes.push_back(reader.ReadU16());
    }
    return nodes;
}

} // namespace

ModelComponent ModelComponent::Read(const object::SerializedObject &object)
{
    ByteReader reader(object.NativeData(), ByteOrder::Big);
    object::PackageIndex model = reader.ReadI32();
    (void)reader.ReadI32(); // zone
    std::size_t element_count = reader.ReadCount(4);
    std::vector<ModelElement> elements;
    elements.reserve(element_count);
    for (std::size_t i = 0; i < element_count; ++i)
    {
        SkipLightMap(reader);
        if (reader.ReadI32() != object.Index())
        {
            reader.Fail("model element belongs to another component");
        }
        ModelElement element;
        element.material = reader.ReadI32();
        element.nodes = ReadNodeList(reader);
        std::size_t shadow_maps = reader.ReadCount(4);
        (void)reader.ReadBytes(shadow_maps * 4U);
        std::size_t irrelevant_lights = reader.ReadCount(kGuidSize);
        (void)reader.ReadBytes(irrelevant_lights * kGuidSize);
        elements.push_back(std::move(element));
    }
    (void)reader.ReadU16(); // component index
    (void)ReadNodeList(reader);
    if (reader.Remaining() != 0U)
    {
        reader.Fail(std::format("{} byte(s) follow the component's node list", reader.Remaining()));
    }
    return {model, std::move(elements)};
}

} // namespace gears::engine::bsp
