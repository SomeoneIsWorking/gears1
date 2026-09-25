#include "level_scene.h"

#include <bit>

#include "object/property_values.h"
#include "object/serialized_object.h"

namespace gears::engine::scene
{
namespace
{

constexpr std::string_view kStaticMeshComponentClass = "Engine.StaticMeshComponent";
constexpr std::string_view kActorClass = "Engine.Actor";

std::uint32_t BigEndian32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (std::uint32_t{bytes[offset]} << 24U) | (std::uint32_t{bytes[offset + 1U]} << 16U) |
           (std::uint32_t{bytes[offset + 2U]} << 8U) | std::uint32_t{bytes[offset + 3U]};
}

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

mesh::Vector3 Scaled(mesh::Vector3 v, float s)
{
    return {v.x * s, v.y * s, v.z * s};
}

// Placement an actor or component stores: location (or translation),
// rotation, and a uniform scale times a per-axis scale.
Matrix Placement(const object::PropertyValues &properties, std::string_view location,
                 std::string_view rotation, std::string_view scale, std::string_view scale3d)
{
    mesh::Vector3 scale_3d = VectorOr(properties, scale3d, {1.0F, 1.0F, 1.0F});
    return ActorPlacement(VectorOr(properties, location, {}), RotatorOf(properties, rotation),
                          Scaled(scale_3d, properties.Float(scale, 1.0F)));
}

std::vector<package::PackageIndex> MaterialOverrides(const object::PropertyValues &properties)
{
    std::span<const std::uint8_t> elements = properties.Array("Materials", 4U);
    std::vector<package::PackageIndex> materials;
    for (std::size_t offset = 0; offset < elements.size(); offset += 4U)
    {
        materials.push_back(static_cast<package::PackageIndex>(BigEndian32(elements, offset)));
    }
    return materials;
}

} // namespace

LevelScene LevelScene::Build(const package::Package &level, object::ClassHierarchy &classes,
                             object::ObjectResolver &resolver)
{
    LevelScene scene;
    const auto &exports = level.Tables().exports;
    for (std::size_t i = 0; i < exports.size(); ++i)
    {
        auto index = static_cast<package::PackageIndex>(i + 1U);
        if ((exports[i].object_flags & object::kObjectFlagClassDefaultObject) != 0U ||
            !classes.IsA(object::ClassHierarchy::ClassPath(level, index),
                         kStaticMeshComponentClass))
        {
            continue;
        }
        ++scene.census_.components;
        if (object::IsInsideClassDefaults(level, i))
        {
            ++scene.census_.templates;
            continue;
        }
        package::PackageIndex owner = exports[i].outer;
        if (owner <= 0 ||
            !classes.IsA(object::ClassHierarchy::ClassPath(level, owner), kActorClass))
        {
            ++scene.census_.outside_actor;
            continue;
        }
        auto component = object::SerializedObject::Read(level, i, classes);
        object::PropertyValues component_properties(component);
        object::Resolution mesh =
            resolver.Resolve(level, component_properties.Object("StaticMesh"));
        if (mesh.status == object::ResolutionStatus::kNull)
        {
            ++scene.census_.without_mesh;
            continue;
        }
        if (mesh.status == object::ResolutionStatus::kCookedOut)
        {
            ++scene.census_.cooked_out_mesh;
            continue;
        }
        auto actor =
            object::SerializedObject::Read(level, static_cast<std::size_t>(owner) - 1U, classes);
        object::PropertyValues actor_properties(actor);
        Matrix local =
            Placement(component_properties, "Translation", "Rotation", "Scale", "Scale3D");
        Matrix world =
            Placement(actor_properties, "Location", "Rotation", "DrawScale", "DrawScale3D");
        scene.instances_.push_back(
            {mesh.location, local * world, MaterialOverrides(component_properties)});
        ++scene.census_.placed;
    }
    return scene;
}

} // namespace gears::engine::scene
