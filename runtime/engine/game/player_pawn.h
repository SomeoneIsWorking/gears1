#pragma once

#include <cstdint>

#include "mesh/static_mesh.h"
#include "pawn_tuning.h"
#include "player_input.h"
#include "scene/world.h"
#include "walking_movement.h"

namespace gears::engine::game
{

// The pawn the player controls: its walking state, the direction its body
// faces, and the control rotation the camera and movement follow.
class PlayerPawn
{
  public:
    // Highest and lowest control pitch, in radians.
    static constexpr float kMaxPitch = 1.2F;

    // Stands at `start` (its location is the capsule's centre), facing and
    // looking along its rotation.
    PlayerPawn(const scene::PlayerStart &start, const PawnTuning &tuning);

    // Turns and tilts the control rotation.
    void Look(const PlayerInput &input);
    // Walks toward the input's movement, relative to the control yaw.
    void Tick(const PlayerInput &input, float seconds, const WalkingMovement &movement);

    [[nodiscard]] const WalkingState &State() const noexcept { return state_; }
    [[nodiscard]] float ControlYaw() const noexcept { return control_yaw_; }
    [[nodiscard]] float ControlPitch() const noexcept { return control_pitch_; }
    [[nodiscard]] float FacingYaw() const noexcept { return facing_yaw_; }

  private:
    const PawnTuning &tuning_;
    WalkingState state_;
    float control_yaw_ = 0.0F;
    float control_pitch_ = 0.0F;
    float facing_yaw_ = 0.0F;
};

// A rotator angle (65536 units per turn) in radians.
float RadiansOf(std::int32_t angle) noexcept;

} // namespace gears::engine::game
