#include "world.h"

#include <cstddef>
#include <string>

#include "object/property_values.h"
#include "object/serialized_object.h"
#include "placement_properties.h"
#include "streaming_levels.h"

namespace gears::engine::scene
{
namespace
{

constexpr std::string_view kPlayerStartClass = "Engine.PlayerStart";

std::optional<PlayerStart> FindPlayerStart(const package::Package &level,
                                           object::ClassHierarchy &classes)
{
    const auto &exports = level.Tables().exports;
    for (std::size_t i = 0; i < exports.size(); ++i)
    {
        auto index = static_cast<package::PackageIndex>(i + 1U);
        if ((exports[i].object_flags & object::kObjectFlagClassDefaultObject) != 0U ||
            object::IsInsideClassDefaults(level, i) ||
            !classes.IsA(object::ClassHierarchy::ClassPath(level, index), kPlayerStartClass))
        {
            continue;
        }
        auto actor = object::SerializedObject::Read(level, i, classes);
        object::PropertyValues properties(actor.Properties());
        return PlayerStart{VectorOr(properties, "Location", {}), RotatorOf(properties, "Rotation")};
    }
    return std::nullopt;
}

} // namespace

World World::Load(package::PackageStore &store, object::ClassHierarchy &classes,
                  object::ObjectResolver &resolver, std::string_view persistent)
{
    World world;
    const package::Package &root = store.Load(persistent);
    std::vector<const package::Package *> packages{&root};
    for (const std::string &name : StreamingLevelPackages(root, classes))
    {
        packages.push_back(&store.Load(name));
    }
    world.levels_.reserve(packages.size());
    for (const package::Package *package : packages)
    {
        world.levels_.push_back({package, LevelScene::Build(*package, classes, resolver)});
        if (!world.start_)
        {
            world.start_ = FindPlayerStart(*package, classes);
        }
    }
    return world;
}

} // namespace gears::engine::scene
