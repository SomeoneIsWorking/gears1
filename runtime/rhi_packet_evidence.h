#pragma once

#include <cstdint>

namespace gears
{

struct RhiBasicDrawPacketEvidence
{
    bool present = false;
    std::uint32_t opcode = 0;
    std::uint32_t primitiveType = 0;
    std::uint32_t sourceSelect = 0;
    std::uint32_t elementCount = 0;
    std::uint32_t headerAddress = 0;
};

template <typename ReadWord>
[[nodiscard]] RhiBasicDrawPacketEvidence
FindLastRhiDrawPacket(std::uint32_t commandEnd, std::uint32_t lowerAddressExclusive,
                      std::uint32_t maximumSearchDwords, ReadWord &&readWord)
{
    constexpr std::uint32_t kDrawIndx = 0x22;
    constexpr std::uint32_t kDrawIndx2 = 0x36;

    for (std::uint32_t distance = 0; distance < maximumSearchDwords; ++distance)
    {
        const std::uint64_t byteDistance = std::uint64_t{distance} * sizeof(std::uint32_t);
        if (byteDistance > commandEnd)
            break;
        const std::uint32_t headerAddress = commandEnd - static_cast<std::uint32_t>(byteDistance);
        if (headerAddress <= lowerAddressExclusive)
            break;

        const std::uint32_t header = readWord(headerAddress);
        if ((header >> 30) != 3)
            continue;
        const std::uint32_t opcode = (header >> 8) & 0x7F;
        if (opcode != kDrawIndx && opcode != kDrawIndx2)
            continue;

        const std::uint32_t payloadDwords = ((header >> 16) & 0x3FFF) + 1;
        const std::uint32_t initiatorIndex = opcode == kDrawIndx ? 1u : 0u;
        if (payloadDwords <= initiatorIndex ||
            std::uint64_t{headerAddress} + std::uint64_t{payloadDwords} * sizeof(std::uint32_t) >
                commandEnd)
        {
            continue;
        }

        const std::uint32_t initiator =
            readWord(headerAddress + (initiatorIndex + 1) * sizeof(std::uint32_t));
        return {
            .present = true,
            .opcode = opcode,
            .primitiveType = initiator & 0x3F,
            .sourceSelect = (initiator >> 6) & 0x3,
            .elementCount = initiator >> 16,
            .headerAddress = headerAddress,
        };
    }
    return {};
}

// The Gears RHI write pointer can move to a newly allocated command buffer
// while a retained operation is emitting packets. A lower post-call pointer
// therefore does not describe an address range that crosses the old buffer's
// end. Keep the normal bounded span first; only after the title-observed
// ordering proves a buffer transition do we search the new buffer's bounded
// tail from its address-space floor.
template <typename ReadWord>
[[nodiscard]] RhiBasicDrawPacketEvidence
FindLastRhiDrawPacketAcrossCommandBuffers(std::uint32_t commandBefore, std::uint32_t commandEnd,
                                          std::uint32_t maximumSearchDwords, ReadWord &&readWord)
{
    const RhiBasicDrawPacketEvidence draw =
        FindLastRhiDrawPacket(commandEnd, commandBefore, maximumSearchDwords, readWord);
    if (draw.present || commandEnd >= commandBefore)
        return draw;
    return FindLastRhiDrawPacket(commandEnd, 0, maximumSearchDwords, readWord);
}

} // namespace gears
