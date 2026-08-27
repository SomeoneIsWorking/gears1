#include "rhi_vertex_buffer.h"

#include <cstdint>
#include <limits>

namespace gears::gears1
{

std::uint32_t EncodeVertexFetchAddress(std::uint32_t guestAddress)
{
    constexpr std::uint32_t kPhysicalAddressMask = 0x1FFFFFFF;
    constexpr std::uint32_t kTopPageMask = 0xFFF;
    constexpr std::uint32_t kTopPageBias = 0x200;
    constexpr std::uint32_t kLocalMemorySelector = 0x1000;
    const std::uint32_t topPage = (guestAddress >> 20) & kTopPageMask;
    return (guestAddress & kPhysicalAddressMask) +
           ((topPage + kTopPageBias) & kLocalMemorySelector);
}

std::optional<RhiSemanticBufferView> DecodeVertexBufferView(std::uint32_t guestAddress,
                                                            std::uint32_t sizeBytes,
                                                            std::uint32_t offsetBytes,
                                                            std::uint32_t strideBytes)
{
    if (offsetBytes > sizeBytes ||
        std::uint64_t{guestAddress} + offsetBytes > std::numeric_limits<std::uint32_t>::max())
    {
        return std::nullopt;
    }

    return RhiSemanticBufferView{
        .allocation = {.guestAddress = EncodeVertexFetchAddress(guestAddress + offsetBytes),
                       .sizeBytes = sizeBytes - offsetBytes},
        .elementStrideBytes = strideBytes,
    };
}

RhiSemanticBufferView DecodeVertexBufferState(std::uint32_t fetchAddress,
                                              std::uint32_t remainingSizeBytes,
                                              std::uint32_t strideDwords)
{
    return {
        .allocation = {.guestAddress = fetchAddress, .sizeBytes = remainingSizeBytes},
        .elementStrideBytes = strideDwords * 4u,
    };
}

} // namespace gears::gears1
