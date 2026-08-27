#include "gpu_swap_packet.h"

#include <cassert>

int main()
{
    const gears::RhiSemanticPresent present{
        .frameSequence = 0x100000011,
        .frontBuffer = 0xA0311000,
        .fetchDescriptor = {10, 20, 30, 40, 50, 60},
    };
    const gears::GpuSwapPacket packet = gears::EncodeGpuSwapPacket(present);
    const gears::RhiPresentPacketEvidence evidence = gears::DecodeGpuSwapPacket(packet);
    assert(evidence.present);
    assert(evidence.framingValid);
    assert(evidence.frameSequence == 0x11);
    assert(evidence.frontBuffer == present.frontBuffer);
    assert(evidence.fetchDescriptor == present.fetchDescriptor);
    assert(gears::CompareRhiPresentPacket(present, evidence) ==
           gears::RhiPresentEvidenceResult::Match);

    gears::GpuSwapPacket malformed = packet;
    malformed[0] ^= 1u << 16;
    assert(!gears::DecodeGpuSwapPacket(malformed).framingValid);

    malformed = packet;
    malformed[6] ^= 1;
    assert(gears::CompareRhiPresentPacket(present, gears::DecodeGpuSwapPacket(malformed)) ==
           gears::RhiPresentEvidenceResult::Mismatch);

    assert(!gears::DecodeGpuSwapPacket(std::span<const std::uint32_t>{packet.data(), 8}).present);
    return 0;
}
