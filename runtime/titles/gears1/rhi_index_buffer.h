#pragma once

#include "rhi_semantic_stream.h"

#include <cstdint>
#include <optional>

namespace gears::gears1
{

[[nodiscard]] RhiSemanticBufferView DecodeIndexBufferView(std::uint32_t commonFlags,
                                                          std::uint32_t guestAddress,
                                                          std::uint32_t sizeBytes);
[[nodiscard]] std::optional<RhiSemanticBufferRange>
IndexBufferSlice(const RhiSemanticBufferView &view, std::uint32_t firstIndex,
                 std::uint32_t indexCount);

} // namespace gears::gears1
