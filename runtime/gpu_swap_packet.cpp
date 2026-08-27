#include "gpu_swap_packet.h"

#include <algorithm>

namespace gears
{

GpuSwapPacket EncodeGpuSwapPacket(const RhiSemanticPresent &present)
{
    GpuSwapPacket packet{};
    packet[0] = (3u << 30) | ((kGpuSwapReservationDwords - 2) << 16) | (kGpuRuntimeSwapOpcode << 8);
    packet[1] = present.frontBuffer;
    packet[2] = static_cast<std::uint32_t>(present.frameSequence);
    std::ranges::copy(present.fetchDescriptor, packet.begin() + 3);
    return packet;
}

RhiPresentPacketEvidence DecodeGpuSwapPacket(std::span<const std::uint32_t> words)
{
    if (words.size() < kGpuSwapReservationDwords)
        return {};

    const std::uint32_t header = words[0];
    RhiPresentPacketEvidence evidence{
        .present = true,
        .framingValid = (header >> 30) == 3 &&
                        ((header >> 16) & 0x3FFF) + 1 == kGpuSwapReservationDwords - 1 &&
                        ((header >> 8) & 0x7F) == kGpuRuntimeSwapOpcode,
        .frameSequence = words[2],
        .frontBuffer = words[1],
    };
    std::ranges::copy_n(words.begin() + 3, evidence.fetchDescriptor.size(),
                        evidence.fetchDescriptor.begin());
    return evidence;
}

} // namespace gears
