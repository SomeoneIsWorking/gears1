#pragma once

#include <cstdint>

namespace gears
{

// Xbox 360 physical RAM is visible through four guest address windows. Clearing
// the top three bits canonicalizes any alias to the shared physical offset.
inline constexpr std::uint32_t kGuestPhysicalAddressMask = 0x1FFFFFFF;

} // namespace gears
