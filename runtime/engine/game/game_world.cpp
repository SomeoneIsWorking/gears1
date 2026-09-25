#include "game_world.h"

#include <algorithm>
#include <format>

#include "package/byte_reader.h"

namespace gears::engine::game
{

const scene::PlayerStart &GameWorld::StartOf(const scene::World &world)
{
    if (!world.Start())
    {
        throw package::PackageFormatError(
            std::format("{} and its sublevels place no PlayerStart", world.Persistent().Name()));
    }
    return *world.Start();
}

GameWorld::GameWorld(const scene::World &world, scene::StaticMeshes &meshes, PawnTuning tuning,
                     CameraRig rig)
    : tuning_(tuning), rig_(rig), collision_(CollisionWorld::Build(world, meshes)),
      movement_(collision_, tuning_), player_(StartOf(world), tuning_)
{
}

void GameWorld::Advance(const PlayerInput &input, float seconds)
{
    player_.Look(input);
    unsimulated_seconds_ += std::min(seconds, kMaxAdvanceSeconds);
    while (unsimulated_seconds_ >= kTickSeconds)
    {
        player_.Tick(input, kTickSeconds, movement_);
        unsimulated_seconds_ -= kTickSeconds;
    }
}

scene::Camera GameWorld::View(float aspect) const
{
    return PlayerView(player_, collision_, rig_, aspect);
}

} // namespace gears::engine::game
