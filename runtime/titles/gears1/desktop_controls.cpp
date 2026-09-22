#include "desktop_controls.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace gears::titles::gears1
{
namespace
{

using x360port::DesktopKey;
using x360port::DesktopMouseButton;

struct KeyBinding
{
    DesktopKey key;
    std::uint16_t button;
};

inline constexpr std::array kKeyBindings{
    KeyBinding{DesktopKey::Space, kPadA},
    KeyBinding{DesktopKey::Return, kPadA},
    KeyBinding{DesktopKey::F, kPadB},
    KeyBinding{DesktopKey::E, kPadX},
    KeyBinding{DesktopKey::Q, kPadY},
    KeyBinding{DesktopKey::R, kPadRightShoulder},
    KeyBinding{DesktopKey::Tab, kPadLeftShoulder},
    KeyBinding{DesktopKey::Escape, kPadStart},
    KeyBinding{DesktopKey::Backspace, kPadBack},
    KeyBinding{DesktopKey::Up, kPadDpadUp},
    KeyBinding{DesktopKey::Right, kPadDpadRight},
    KeyBinding{DesktopKey::Down, kPadDpadDown},
    KeyBinding{DesktopKey::Left, kPadDpadLeft},
    KeyBinding{DesktopKey::Digit1, kPadDpadUp},
    KeyBinding{DesktopKey::Digit2, kPadDpadRight},
    KeyBinding{DesktopKey::Digit3, kPadDpadDown},
    KeyBinding{DesktopKey::Digit4, kPadDpadLeft},
};

inline constexpr std::int32_t kFullDeflection = 32767;
inline constexpr std::uint8_t kFullTrigger = 255;

// Opposing keys cancel, as they would on a stick pushed both ways at once.
[[nodiscard]] std::int16_t KeyAxis(const x360port::DesktopSnapshot &desktop, DesktopKey negative,
                                   DesktopKey positive) noexcept
{
    std::int32_t value = 0;
    if (desktop.IsKeyDown(positive))
    {
        value += kFullDeflection;
    }
    if (desktop.IsKeyDown(negative))
    {
        value -= kFullDeflection;
    }
    return static_cast<std::int16_t>(value);
}

[[nodiscard]] std::int16_t MouseAxis(std::int32_t travel) noexcept
{
    if (travel == 0)
    {
        return 0;
    }
    std::int64_t magnitude =
        kRightThumbDeadZone + std::int64_t{kStickUnitsPerPixel} * std::abs(std::int64_t{travel});
    std::int64_t deflection = std::min<std::int64_t>(magnitude, kFullDeflection);
    return static_cast<std::int16_t>(travel < 0 ? -deflection : deflection);
}

} // namespace

PadState MapDesktopControls(const x360port::DesktopSnapshot &desktop) noexcept
{
    PadState pad;
    for (const KeyBinding &binding : kKeyBindings)
    {
        if (desktop.IsKeyDown(binding.key))
        {
            pad.buttons |= binding.button;
        }
    }
    if (desktop.IsButtonDown(DesktopMouseButton::Middle))
    {
        pad.buttons |= kPadRightThumb;
    }
    if (desktop.IsButtonDown(DesktopMouseButton::Left))
    {
        pad.rightTrigger = kFullTrigger;
    }
    if (desktop.IsButtonDown(DesktopMouseButton::Right))
    {
        pad.leftTrigger = kFullTrigger;
    }
    pad.thumbLX = KeyAxis(desktop, DesktopKey::A, DesktopKey::D);
    pad.thumbLY = KeyAxis(desktop, DesktopKey::S, DesktopKey::W);
    // The console's stick Y points up; the desktop's pointer Y points down.
    pad.thumbRX = MouseAxis(desktop.pointer_dx);
    pad.thumbRY = MouseAxis(-desktop.pointer_dy);
    return pad;
}

PadState SampleDesktopControls(void *desktop_input_state) noexcept
{
    return MapDesktopControls(
        static_cast<x360port::DesktopInputState *>(desktop_input_state)->TakeSnapshot());
}

} // namespace gears::titles::gears1
