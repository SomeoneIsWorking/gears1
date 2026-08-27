#include "rhi_index_buffer.h"

#include <cstdint>
#include <limits>

namespace gears::gears1
{

RhiSemanticBufferView DecodeIndexBufferView(std::uint32_t commonFlags, std::uint32_t guestAddress,
                                            std::uint32_t sizeBytes)
{
    constexpr std::uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
    return {
        .allocation = {.guestAddress = guestAddress & kPhysicalAddressMask, .sizeBytes = sizeBytes},
        .elementStrideBytes = (commonFlags & 0x80000000u) != 0 ? 4u : 2u,
        .endianSwap = (commonFlags >> 29) & 3u,
    };
}

std::optional<RhiSemanticBufferRange> IndexBufferSlice(const RhiSemanticBufferView &view,
                                                       std::uint32_t firstIndex,
                                                       std::uint32_t indexCount)
{
    if (view.elementStrideBytes != 2 && view.elementStrideBytes != 4)
        return std::nullopt;

    const std::uint64_t offset = std::uint64_t{firstIndex} * std::uint64_t{view.elementStrideBytes};
    const std::uint64_t size = std::uint64_t{indexCount} * std::uint64_t{view.elementStrideBytes};
    const std::uint64_t end = offset + size;
    if (end > view.allocation.sizeBytes || std::uint64_t{view.allocation.guestAddress} + end >
                                               std::numeric_limits<std::uint32_t>::max())
    {
        return std::nullopt;
    }

    return RhiSemanticBufferRange{
        .guestAddress = view.allocation.guestAddress + static_cast<std::uint32_t>(offset),
        .sizeBytes = static_cast<std::uint32_t>(size),
    };
}

} // namespace gears::gears1
