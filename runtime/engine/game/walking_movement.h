#pragma once

#include <optional>

#include "collision_world.h"
#include "mesh/static_mesh.h"
#include "pawn_tuning.h"

namespace gears::engine::game
{

// Where a walking pawn is and how it moves: its capsule's centre, its
// velocity, and whether it stands on a walkable floor.
struct WalkingState
{
    mesh::Vector3 location;
    mesh::Vector3 velocity;
    bool on_ground = false;
};

// Moves a vertical capsule through the collision world: accelerates toward
// the wished horizontal velocity, falls under gravity when unsupported,
// slides along what it hits, walks up steps, and keeps to the floor it walks
// on. The capsule is swept as three spheres along its axis.
class WalkingMovement
{
  public:
    WalkingMovement(const CollisionWorld &collision, const PawnTuning &tuning)
        : collision_(collision), tuning_(tuning)
    {
    }

    // Advances `state` by `seconds` toward `wish_velocity` (horizontal).
    void Tick(WalkingState &state, mesh::Vector3 wish_velocity, float seconds) const;

    // The capsule at `location` swept by `delta`: its first contact.
    [[nodiscard]] std::optional<SweepHit> SweepCapsule(mesh::Vector3 location,
                                                       mesh::Vector3 delta) const;

  private:
    // Moves by `delta`, sliding along whatever is hit; returns whether a wall
    // too steep to stand on blocked it.
    bool SlideMove(WalkingState &state, mesh::Vector3 delta) const;
    // Retries a blocked horizontal move one step higher, then settles back
    // down; keeps the result only when it went further.
    void StepUp(WalkingState &state, mesh::Vector3 horizontal) const;
    // Sticks to a floor within a step below, or leaves the pawn falling.
    void FindFloor(WalkingState &state) const;

    const CollisionWorld &collision_;
    const PawnTuning &tuning_;
};

} // namespace gears::engine::game
