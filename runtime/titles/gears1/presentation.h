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

// The PC product's frame-rate ceiling (project state S013).
inline constexpr std::uint32_t kTargetPresentsPerSecond = 120;

inline constexpr std::uint32_t kDisplayRefreshHz = kVblanksPerPresent * kTargetPresentsPerSecond;

} // namespace gears::titles::gears1
