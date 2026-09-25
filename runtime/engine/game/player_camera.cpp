#include "player_camera.h"

#include <algorithm>
#include <cmath>

#include "mesh/vector_math.h"

namespace gears::engine::game
{
namespace
{

// How far ahead of the eye the camera aims.
constexpr float kAimDistance = 1000.0F;
// Distance kept between the pulled-in camera and what blocked it.
constexpr float kWallGap = 2.0F;

} // namespace

scene::Camera PlayerView(const PlayerPawn &pawn, const CollisionWorld &collision,
                         const CameraRig &rig, float aspect)
{
    float yaw = pawn.ControlYaw();
    float pitch = pawn.ControlPitch();
    mesh::Vector3 forward{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw),
                          std::sin(pitch)};
    mesh::Vector3 right{-std::sin(yaw), std::cos(yaw), 0.0F};
    mesh::Vector3 pivot = pawn.State().location + mesh::Vector3{0.0F, 0.0F, rig.pivot_height};
    mesh::Vector3 offset =
        forward * -rig.distance + right * rig.shoulder + mesh::Vector3{0.0F, 0.0F, rig.lift};
    std::optional<SweepHit> blocked = collision.SweepSphere(pivot, offset, rig.probe_radius);
    float reach = 1.0F;
    if (blocked)
    {
        float length = mesh::Length(offset);
        reach = std::max(blocked->time * length - kWallGap, 0.0F) / length;
    }
    scene::Camera camera;
    camera.eye = pivot + offset * reach;
    camera.target = camera.eye + forward * kAimDistance;
    camera.vertical_fov_radians = rig.vertical_fov_radians;
    camera.aspect = aspect;
    return camera;
}

} // namespace gears::engine::game
