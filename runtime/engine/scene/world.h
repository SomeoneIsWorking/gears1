#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "level_scene.h"
#include "object/class_hierarchy.h"
#include "object/object_resolver.h"
#include "package/package.h"
#include "package/package_store.h"
#include "transform.h"

namespace gears::engine::scene
{

// One loaded level package and the geometry it places.
struct WorldLevel
{
    const package::Package *package = nullptr;
    LevelScene scene;
};

// Where a player enters the world: a PlayerStart's location and facing.
struct PlayerStart
{
    mesh::Vector3 location;
    Rotator rotation;
};

// A persistent level and every sublevel its WorldInfo streams, all loaded
// at once, persistent level first.
class World
{
  public:
    // Loads `persistent` and its sublevels through `store`. Refuses a level
    // whose package or streaming entries do not decode.
    static World Load(package::PackageStore &store, object::ClassHierarchy &classes,
                      object::ObjectResolver &resolver, std::string_view persistent);

    [[nodiscard]] const std::vector<WorldLevel> &Levels() const noexcept { return levels_; }
    [[nodiscard]] const package::Package &Persistent() const noexcept
    {
        return *levels_.front().package;
    }
    // The first PlayerStart of the persistent level, else of the sublevels
    // in streaming order; none when no level places one.
    [[nodiscard]] const std::optional<PlayerStart> &Start() const noexcept { return start_; }

  private:
    std::vector<WorldLevel> levels_;
    std::optional<PlayerStart> start_;
};

} // namespace gears::engine::scene
