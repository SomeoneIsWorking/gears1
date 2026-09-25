#pragma once

#include "collision_world.h"
#include "pawn_tuning.h"
#include "player_camera.h"
#include "player_input.h"
#include "player_pawn.h"
#include "scene/camera.h"
#include "scene/static_meshes.h"
#include "scene/world.h"
#include "walking_movement.h"

namespace gears::engine::game
{

// The running game: the loaded world's collision, the player pawn standing
// at its PlayerStart, and a fixed-step simulation clock.
class GameWorld
{
  public:
    // Simulation steps per second; movement advances only in whole steps.
    static constexpr float kTicksPerSecond = 60.0F;
    static constexpr float kTickSeconds = 1.0F / kTicksPerSecond;
    // Longest real time one Advance simulates, so a stall does not replay
    // seconds of ticks at once.
    static constexpr float kMaxAdvanceSeconds = 0.25F;

    // Refuses a world that places no PlayerStart.
    GameWorld(const scene::World &world, scene::StaticMeshes &meshes, PawnTuning tuning,
              CameraRig rig);

    // Applies this frame's look at once, then simulates `seconds` of real
    // time in fixed ticks under this frame's movement.
    void Advance(const PlayerInput &input, float seconds);

    [[nodiscard]] scene::Camera View(float aspect) const;
    [[nodiscard]] const PlayerPawn &Player() const noexcept { return player_; }
    [[nodiscard]] const CollisionWorld &Collision() const noexcept { return collision_; }

  private:
    [[nodiscard]] static const scene::PlayerStart &StartOf(const scene::World &world);

    PawnTuning tuning_;
    CameraRig rig_;
    CollisionWorld collision_;
    WalkingMovement movement_;
    PlayerPawn player_;
    float unsimulated_seconds_ = 0.0F;
};

} // namespace gears::engine::game
