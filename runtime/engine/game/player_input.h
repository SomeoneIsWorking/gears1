#pragma once

namespace gears::engine::game
{

// One tick's player intent, in the game's terms rather than a device's:
// device sources (keyboard and mouse, gamepad) translate into it.
struct PlayerInput
{
    // Movement relative to where the camera faces, each in [-1, 1].
    float move_forward = 0.0F;
    float move_right = 0.0F;
    // Camera turn and look this tick, in radians; positive turns right and
    // looks up.
    float turn = 0.0F;
    float look_up = 0.0F;
    // The run action held: a roadie run while moving.
    bool roadie_run = false;
};

} // namespace gears::engine::game
