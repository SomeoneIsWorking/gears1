#include "player_pawn.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "mesh/vector_math.h"

namespace gears::engine::game
{
namespace
{

constexpr float kTurn = 2.0F * std::numbers::pi_v<float>;
constexpr float kAnglesPerTurn = 65536.0F;

// The signed difference `to - from` wrapped into [-pi, pi].
float AngleBetween(float from, float to)
{
    return std::remainder(to - from, kTurn);
}

} // namespace

float RadiansOf(std::int32_t angle) noexcept
{
    return static_cast<float>(angle) * (kTurn / kAnglesPerTurn);
}

PlayerPawn::PlayerPawn(const scene::PlayerStart &start, const PawnTuning &tuning)
    : tuning_(tuning), control_yaw_(RadiansOf(start.rotation.yaw)),
      control_pitch_(std::clamp(RadiansOf(start.rotation.pitch), -kMaxPitch, kMaxPitch)),
      facing_yaw_(control_yaw_)
{
    state_.location = start.location;
}

void PlayerPawn::Look(const PlayerInput &input)
{
    control_yaw_ = std::remainder(control_yaw_ + input.turn, kTurn);
    control_pitch_ = std::clamp(control_pitch_ + input.look_up, -kMaxPitch, kMaxPitch);
}

void PlayerPawn::Tick(const PlayerInput &input, float seconds, const WalkingMovement &movement)
{
    mesh::Vector3 forward{std::cos(control_yaw_), std::sin(control_yaw_), 0.0F};
    mesh::Vector3 right{-std::sin(control_yaw_), std::cos(control_yaw_), 0.0F};
    mesh::Vector3 wish = forward * input.move_forward + right * input.move_right;
    float amount = std::min(mesh::Length(wish), 1.0F);
    mesh::Vector3 direction = mesh::NormalizedOrZero(wish);
    float speed = input.roadie_run ? tuning_.roadie_run_speed : tuning_.run_speed;
    movement.Tick(state_, direction * (speed * amount), seconds);

    // The body turns toward where it moves.
    if (amount > 0.0F)
    {
        float target = std::atan2(direction.y, direction.x);
        float difference = AngleBetween(facing_yaw_, target);
        float step = tuning_.turn_rate * seconds;
        facing_yaw_ = std::remainder(facing_yaw_ + std::clamp(difference, -step, step), kTurn);
    }
}

} // namespace gears::engine::game
