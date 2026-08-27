#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rhi_semantic_stream.h"

namespace gears
{

inline constexpr std::uint32_t kGpuRuntimeSwapOpcode = 0x7F;
inline constexpr std::size_t kGpuSwapReservationDwords = 64;
using GpuSwapPacket = std::array<std::uint32_t, kGpuSwapReservationDwords>;

[[nodiscard]] GpuSwapPacket EncodeGpuSwapPacket(const RhiSemanticPresent &present);
[[nodiscard]] RhiPresentPacketEvidence DecodeGpuSwapPacket(std::span<const std::uint32_t> words);

} // namespace gears
