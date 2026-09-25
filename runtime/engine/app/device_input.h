#pragma once

#include "game/player_input.h"

struct SDL_Gamepad;

namespace gears::engine::app
{

struct InputSettings
{
    // Look per pixel of mouse motion, in radians.
    float mouse_radians_per_pixel = 0.0025F;
    // Look at full stick deflection, in radians per second.
    float stick_radians_per_second = 3.0F;
    // Stick deflection below this reads as centred.
    float stick_dead_zone = 0.24F;
};

// Keyboard, mouse, and the first connected gamepad, translated into the
// game's player input: WASD or the left stick move, the mouse or the right
// stick look, Left Shift or the gamepad's south button (A) roadie-runs.
// Escape asks to leave the game.
class DeviceInput
{
  public:
    explicit DeviceInput(InputSettings settings) : settings_(settings) {}
    ~DeviceInput();
    DeviceInput(const DeviceInput &) = delete;
    DeviceInput &operator=(const DeviceInput &) = delete;

    // Drains pending window events; false once the player asked to quit.
    [[nodiscard]] bool Poll();
    // The input gathered since the last read, with stick look scaled to
    // `seconds` of frame time.
    [[nodiscard]] game::PlayerInput Read(float seconds);

  private:
    [[nodiscard]] float Axis(int axis) const;

    InputSettings settings_;
    SDL_Gamepad *gamepad_ = nullptr;
    float mouse_x_ = 0.0F;
    float mouse_y_ = 0.0F;
    bool quit_ = false;
};

} // namespace gears::engine::app
