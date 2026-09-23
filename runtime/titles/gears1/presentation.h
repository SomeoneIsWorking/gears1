#pragma once

#include <cstdint>

namespace gears::titles::gears1
{

// Gears of War presents on every second vblank: 30 presents a second at the
// console's 60 Hz. Its game clock is the host clock, not the vblank count: with
// the vblank at 200 Hz the profile's gameplay walk presented about 99 times a
// second through the menus and Act 1's opening and reached the same scene at the
// same timestamps as at 60 Hz. The console's vblank rate therefore sets only how
// often the title presents.
inline constexpr std::uint32_t kVblanksPerPresent = 2;

// The PC product's frame-rate ceiling (project state S013), enforced on the
// host at each present rather than by the vblank rate.
inline constexpr std::uint32_t kTargetPresentsPerSecond = 120;

// The title waits for a vblank at every present, so vblank pacing rounds each
// frame up to the vblank grid. At 240 Hz a 12 ms frame at the path junction
// became 16.7 ms; the fastest vblank the platform runs keeps that rounding
// under a millisecond, and the host limit above caps the rate instead
// (headless gameplay walk, 2026-09-23).
inline constexpr std::uint32_t kDisplayRefreshHz = 1000;

static_assert(kDisplayRefreshHz >= kVblanksPerPresent * kTargetPresentsPerSecond,
              "the vblank rate must not cap presents below the host limit");

} // namespace gears::titles::gears1
