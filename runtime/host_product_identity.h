#pragma once

namespace gears
{

// Shared host-facing identity belongs to the GearsUE3 engine, not whichever
// exact title happens to be linked as the current conformance target. Exact
// title and revision identity remain in TitleProfile.
inline constexpr char kHostProductName[] = "GearsUE3";
inline constexpr char kHostProductKey[] = "gearsue3";
inline constexpr char kHostDrawApplicationName[] = "gearsue3-draw";

} // namespace gears
