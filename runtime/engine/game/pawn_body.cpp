#include "pawn_body.h"

#include <cmath>

namespace gears::engine::game
{

scene::Matrix BodyPlacement(const PlayerPawn &pawn, const PawnAppearance &appearance,
                            const mesh::SkeletalMesh &mesh)
{
    const auto &origin = mesh.OriginRotation();
    scene::Matrix component = scene::ActorPlacement(
        appearance.translation, scene::Rotator{origin[0], origin[1], origin[2]},
        mesh::Vector3{1.0F, 1.0F, 1.0F});
    float sy = std::sin(pawn.FacingYaw());
    float cy = std::cos(pawn.FacingYaw());
    scene::Matrix actor = scene::Matrix::Identity();
    actor.m[0] = {cy, sy, 0.0F, 0.0F};
    actor.m[1] = {-sy, cy, 0.0F, 0.0F};
    const mesh::Vector3 &location = pawn.State().location;
    actor.m[3] = {location.x, location.y, location.z, 1.0F};
    return component * actor;
}

} // namespace gears::engine::game
