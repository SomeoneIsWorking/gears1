#include "device_input.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>

namespace gears::engine::app
{
namespace
{

constexpr float kAxisRange = 32767.0F;

float KeyAxis(const bool *keys, SDL_Scancode positive, SDL_Scancode negative)
{
    return (keys[positive] ? 1.0F : 0.0F) - (keys[negative] ? 1.0F : 0.0F);
}

} // namespace

DeviceInput::~DeviceInput()
{
    if (gamepad_ != nullptr)
    {
        SDL_CloseGamepad(gamepad_);
    }
}

bool DeviceInput::Poll()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_EVENT_QUIT:
            quit_ = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.scancode == SDL_SCANCODE_ESCAPE)
            {
                quit_ = true;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            mouse_x_ += event.motion.xrel;
            mouse_y_ += event.motion.yrel;
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (gamepad_ == nullptr)
            {
                gamepad_ = SDL_OpenGamepad(event.gdevice.which);
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (gamepad_ != nullptr && SDL_GetGamepadID(gamepad_) == event.gdevice.which)
            {
                SDL_CloseGamepad(gamepad_);
                gamepad_ = nullptr;
            }
            break;
        default:
            break;
        }
    }
    return !quit_;
}

float DeviceInput::Axis(int axis) const
{
    if (gamepad_ == nullptr)
    {
        return 0.0F;
    }
    float value =
        static_cast<float>(SDL_GetGamepadAxis(gamepad_, static_cast<SDL_GamepadAxis>(axis))) /
        kAxisRange;
    float magnitude = std::fabs(value);
    if (magnitude < settings_.stick_dead_zone)
    {
        return 0.0F;
    }
    // Rescale so movement starts from zero at the dead zone's edge.
    float scaled = (magnitude - settings_.stick_dead_zone) / (1.0F - settings_.stick_dead_zone);
    return std::copysign(std::min(scaled, 1.0F), value);
}

game::PlayerInput DeviceInput::Read(float seconds)
{
    const bool *keys = SDL_GetKeyboardState(nullptr);
    game::PlayerInput input;
    input.move_forward = std::clamp(
        KeyAxis(keys, SDL_SCANCODE_W, SDL_SCANCODE_S) - Axis(SDL_GAMEPAD_AXIS_LEFTY), -1.0F, 1.0F);
    input.move_right = std::clamp(
        KeyAxis(keys, SDL_SCANCODE_D, SDL_SCANCODE_A) + Axis(SDL_GAMEPAD_AXIS_LEFTX), -1.0F, 1.0F);
    float stick_look = settings_.stick_radians_per_second * seconds;
    input.turn =
        mouse_x_ * settings_.mouse_radians_per_pixel + Axis(SDL_GAMEPAD_AXIS_RIGHTX) * stick_look;
    input.look_up =
        -mouse_y_ * settings_.mouse_radians_per_pixel - Axis(SDL_GAMEPAD_AXIS_RIGHTY) * stick_look;
    input.roadie_run =
        keys[SDL_SCANCODE_LSHIFT] ||
        (gamepad_ != nullptr && SDL_GetGamepadButton(gamepad_, SDL_GAMEPAD_BUTTON_SOUTH));
    mouse_x_ = 0.0F;
    mouse_y_ = 0.0F;
    return input;
}

} // namespace gears::engine::app
