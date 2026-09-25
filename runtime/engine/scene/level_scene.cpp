#include "level_scene.h"

#include "bsp/component_geometry.h"
#include "object/property_values.h"
#include "object/serialized_object.h"
#include "placement_properties.h"

namespace gears::engine::scene
{
namespace
{

constexpr std::string_view kStaticMeshComponentClass = "Engine.StaticMeshComponent";
constexpr std::string_view kActorClass = "Engine.Actor";
constexpr std::string_view kModelComponentClass = "Engine.ModelComponent";

} // namespace

LevelScene LevelScene::Build(const package::Package &level, object::ClassHierarchy &classes,
                             object::ObjectResolver &resolver)
{
    LevelScene scene;
    bsp::ComponentGeometry geometry(classes, resolver);
    const auto &exports = level.Tables().exports;
    for (std::size_t i = 0; i < exports.size(); ++i)
    {
        auto index = static_cast<package::PackageIndex>(i + 1U);
        if ((exports[i].object_flags & object::kObjectFlagClassDefaultObject) != 0U)
        {
            continue;
        }
        std::string class_path = object::ClassHierarchy::ClassPath(level, index);
        if (classes.IsA(class_path, kModelComponentClass) &&
            !object::IsInsideClassDefaults(level, i))
        {
            auto component = object::SerializedObject::Read(level, i, classes);
            scene.models_.push_back({&level, geometry.Triangulate(component)});
            ++scene.census_.model_components;
            continue;
        }
        if (!classes.IsA(class_path, kStaticMeshComponentClass))
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
        object::PropertyValues component_properties(component.Properties());
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
        object::PropertyValues actor_properties(actor.Properties());
        Matrix local =
            Placement(component_properties, "Translation", "Rotation", "Scale", "Scale3D");
        Matrix world =
            Placement(actor_properties, "Location", "Rotation", "DrawScale", "DrawScale3D");
        scene.instances_.push_back(
            {mesh.location, local * world, component_properties.ObjectArray("Materials")});
        ++scene.census_.placed;
    }
    return scene;
}

} // namespace gears::engine::scene
