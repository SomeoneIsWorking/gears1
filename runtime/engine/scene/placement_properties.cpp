#include "placement_properties.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gears::engine::scene
{
namespace
{

std::uint32_t BigEndian32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (std::uint32_t{bytes[offset]} << 24U) | (std::uint32_t{bytes[offset + 1U]} << 16U) |
           (std::uint32_t{bytes[offset + 2U]} << 8U) | std::uint32_t{bytes[offset + 3U]};
}

mesh::Vector3 Scaled(mesh::Vector3 v, float s)
{
    return {v.x * s, v.y * s, v.z * s};
}

} // namespace

mesh::Vector3 VectorOr(const object::PropertyValues &properties, std::string_view name,
                       mesh::Vector3 fallback)
{
    std::span<const std::uint8_t> value = properties.Struct(name, "Vector", 12U);
    if (value.empty())
    {
        return fallback;
    }
    return {std::bit_cast<float>(BigEndian32(value, 0U)),
            std::bit_cast<float>(BigEndian32(value, 4U)),
            std::bit_cast<float>(BigEndian32(value, 8U))};
}

Rotator RotatorOf(const object::PropertyValues &properties, std::string_view name)
{
    std::span<const std::uint8_t> value = properties.Struct(name, "Rotator", 12U);
    if (value.empty())
    {
        return {};
    }
    return {static_cast<std::int32_t>(BigEndian32(value, 0U)),
            static_cast<std::int32_t>(BigEndian32(value, 4U)),
            static_cast<std::int32_t>(BigEndian32(value, 8U))};
}

Matrix Placement(const object::PropertyValues &properties, std::string_view location,
                 std::string_view rotation, std::string_view scale, std::string_view scale3d)
{
    mesh::Vector3 scale_3d = VectorOr(properties, scale3d, {1.0F, 1.0F, 1.0F});
    return ActorPlacement(VectorOr(properties, location, {}), RotatorOf(properties, rotation),
                          Scaled(scale_3d, properties.Float(scale, 1.0F)));
}

} // namespace gears::engine::scene
