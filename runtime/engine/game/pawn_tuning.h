#pragma once

namespace gears::engine::game
{

// The player pawn's collision shape and movement rates, in world units and
// seconds. These are Unreal's stock pawn values (cylinder radius and half
// height, walking speed, the world's default gravity), not yet the title's
// own: reading them from the Gears pawn class's defaults is issue 0180.
struct PawnTuning
{
    float radius = 34.0F;
    float half_height = 72.0F;
    float run_speed = 440.0F;
    float roadie_run_speed = 720.0F;
    float acceleration = 2048.0F;
    float gravity = -520.0F;
    // The highest ledge the pawn walks up without stopping.
    float step_height = 35.0F;
    // The steepest floor the pawn stands on: the lowest normal Z.
    float walkable_floor_z = 0.7F;
    // How fast the pawn turns to face where it moves, in radians per second.
    float turn_rate = 10.0F;
};

} // namespace gears::engine::game
