#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gears
{

inline constexpr std::size_t kRhiTextureSlotCount = 32;
inline constexpr std::size_t kRhiTextureDescriptorDwords = 6;
using RhiTextureDescriptor = std::array<std::uint32_t, kRhiTextureDescriptorDwords>;
using RhiTextureFetchState = std::array<RhiTextureDescriptor, kRhiTextureSlotCount>;

} // namespace gears
