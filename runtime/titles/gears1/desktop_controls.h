#pragma once

#include <cstdint>

#include <x360port/desktop_input.hpp>

#include "input.h"

namespace gears::titles::gears1
{

// XInput's documented right-thumb dead zone. Mouse travel starts just beyond
// it, so the slowest aim still turns the camera instead of vanishing into the
// stick's dead zone.
inline constexpr std::int32_t kRightThumbDeadZone = 8689;
// Stick units per pixel of mouse travel between two polls, beyond the dead
// zone. Full deflection is about 24 pixels per poll.
inline constexpr std::int32_t kStickUnitsPerPixel = 1000;

// The PC controls for Gears of War: W/A/S/D move, the mouse aims, left click
// fires, right click aims down the sights, middle click zooms; Space takes
// cover and runs, E uses, F melees, R reloads, Q looks at a point of interest,
// Tab shows objectives; 1-4 and the arrow keys are the d-pad; Escape pauses,
// Backspace is Back, and Return confirms.
[[nodiscard]] PadState MapDesktopControls(const x360port::DesktopSnapshot &desktop) noexcept;

// A HostPadSampler over a DesktopInputState: takes a snapshot and maps it.
[[nodiscard]] PadState SampleDesktopControls(void *desktop_input_state) noexcept;

} // namespace gears::titles::gears1
