#pragma once

#include "collision_world.h"
#include "player_pawn.h"
#include "scene/camera.h"

namespace gears::engine::game
{

// Where the over-the-shoulder camera sits relative to the pawn: a pivot
// above the capsule's centre, then back along the control rotation, out to
// the right shoulder, and up.
struct CameraRig
{
    float pivot_height = 48.0F;
    float distance = 170.0F;
    float shoulder = 45.0F;
    float lift = 12.0F;
    // The sphere the camera sweeps from the pivot so it never sits inside a
    // wall.
    float probe_radius = 12.0F;
    float vertical_fov_radians = 1.1F;
};

// The third-person view of the player pawn, pulled in when the level stands
// between the pivot and the rigged position.
scene::Camera PlayerView(const PlayerPawn &pawn, const CollisionWorld &collision,
                         const CameraRig &rig, float aspect);

} // namespace gears::engine::game
