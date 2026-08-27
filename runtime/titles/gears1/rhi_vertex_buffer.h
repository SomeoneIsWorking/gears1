#pragma once

#include "rhi_semantic_stream.h"

#include <cstdint>
#include <optional>

namespace gears::gears1
{

inline constexpr std::uint32_t kVertexStreamSlotCount = 16;

[[nodiscard]] std::uint32_t EncodeVertexFetchAddress(std::uint32_t guestAddress);
[[nodiscard]] std::optional<RhiSemanticBufferView>
DecodeVertexBufferView(std::uint32_t guestAddress, std::uint32_t sizeBytes,
                       std::uint32_t offsetBytes, std::uint32_t strideBytes);
[[nodiscard]] RhiSemanticBufferView DecodeVertexBufferState(std::uint32_t fetchAddress,
                                                            std::uint32_t remainingSizeBytes,
                                                            std::uint32_t strideDwords);

} // namespace gears::gears1
