#pragma once

#include <cstdint>
#include <vector>

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

struct RhiShaderLoadPacketEvidence
{
    std::uint32_t stage = 0;
    std::uint32_t guestAddress = 0;
    std::uint32_t sizeBytes = 0;
    std::uint32_t headerAddress = 0;
    bool predicated = false;
};

struct RhiShaderLoadRangeEvidence
{
    bool complete = true;
    std::uint32_t packetCount = 0;
    std::uint32_t dwordCount = 0;
    std::vector<RhiShaderLoadPacketEvidence> loads;
};

// Parses the exact packet span appended after a retained command-buffer
// cursor. The cursor points at the previous final dword, while commandEnd is
// the new final dword. Refuse malformed or truncated spans so arbitrary data
// cannot masquerade as shader evidence.
template <typename ReadWord>
[[nodiscard]] RhiShaderLoadRangeEvidence InspectRhiShaderLoadRange(std::uint32_t commandBefore,
                                                                   std::uint32_t commandEnd,
                                                                   ReadWord &&readWord)
{
    constexpr std::uint32_t kType3 = 3;
    constexpr std::uint32_t kImLoad = 0x27;

    RhiShaderLoadRangeEvidence result;
    if ((commandBefore & 3) != 0 || (commandEnd & 3) != 0 || commandEnd < commandBefore)
    {
        result.complete = false;
        return result;
    }

    std::uint64_t cursor = std::uint64_t{commandBefore} + sizeof(std::uint32_t);
    const std::uint64_t end = commandEnd;
    while (cursor <= end)
    {
        const std::uint32_t headerAddress = static_cast<std::uint32_t>(cursor);
        const std::uint32_t header = readWord(headerAddress);
        const std::uint32_t type = header >> 30;
        std::uint32_t payloadDwords = 0;
        if (type == 0 || type == kType3)
            payloadDwords = ((header >> 16) & 0x3FFF) + 1;
        else if (type == 1)
            payloadDwords = 2;

        const std::uint64_t packetDwords = std::uint64_t{payloadDwords} + 1;
        const std::uint64_t next = cursor + packetDwords * sizeof(std::uint32_t);
        if (next == 0 || next - sizeof(std::uint32_t) > end)
        {
            result.complete = false;
            return result;
        }

        ++result.packetCount;
        result.dwordCount += static_cast<std::uint32_t>(packetDwords);
        if (type == kType3 && ((header >> 8) & 0x7F) == kImLoad)
        {
            if (payloadDwords != 2)
            {
                result.complete = false;
                return result;
            }
            const std::uint32_t addressAndStage = readWord(headerAddress + 4);
            const std::uint32_t startAndSize = readWord(headerAddress + 8);
            const std::uint32_t stage = addressAndStage & 3;
            const std::uint32_t start = startAndSize >> 16;
            const std::uint32_t sizeDwords = startAndSize & 0xFFFF;
            if (stage > 1 || start != 0 || sizeDwords == 0 || sizeDwords % 3 != 0 ||
                sizeDwords > 0x4000)
            {
                result.complete = false;
                return result;
            }
            result.loads.push_back({
                .stage = stage,
                .guestAddress = addressAndStage & ~std::uint32_t{3},
                .sizeBytes = sizeDwords * std::uint32_t{sizeof(std::uint32_t)},
                .headerAddress = headerAddress,
                .predicated = (header & 1) != 0,
            });
        }
        cursor = next;
    }
    return result;
}

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
