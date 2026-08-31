#include "rhi_packet_evidence.h"
#include "titles/gears1/shader_flush_capture.h"

#include <array>
#include <cassert>
#include <cstdint>

namespace
{

[[nodiscard]] constexpr std::uint32_t Type3Header(std::uint32_t opcode, std::uint32_t payloadDwords,
                                                  bool predicated = false)
{
    return 0xC0000000u | ((payloadDwords - 1) << 16) | (opcode << 8) |
           static_cast<std::uint32_t>(predicated);
}

void TestExactShaderLoads()
{
    constexpr std::uint32_t kBase = 0x1000;
    constexpr std::array<std::uint32_t, 9> words{
        0x80000000,
        Type3Header(0x10, 1),
        0,
        Type3Header(0x27, 2),
        0x00120000,
        12,
        Type3Header(0x27, 2, true),
        0x00130001,
        18,
    };
    const auto read = [&](std::uint32_t address)
    {
        assert(address >= kBase && (address - kBase) / 4 < words.size());
        return words[(address - kBase) / 4];
    };
    const gears::RhiShaderLoadRangeEvidence evidence =
        gears::InspectRhiShaderLoadRange(kBase - 4, kBase + 32, read);
    assert(evidence.complete);
    assert(evidence.packetCount == 4);
    assert(evidence.dwordCount == words.size());
    assert(evidence.loads.size() == 2);
    assert(evidence.loads[0].stage == 0);
    assert(evidence.loads[0].guestAddress == 0x00120000);
    assert(evidence.loads[0].sizeBytes == 48);
    assert(!evidence.loads[0].predicated);
    assert(evidence.loads[1].stage == 1);
    assert(evidence.loads[1].guestAddress == 0x00130000);
    assert(evidence.loads[1].sizeBytes == 72);
    assert(evidence.loads[1].predicated);
}

void TestNegativeAndZeroEvidence()
{
    constexpr std::uint32_t kBase = 0x2000;
    constexpr std::array<std::uint32_t, 3> malformed{
        Type3Header(0x27, 2),
        0x00120000,
        10,
    };
    const auto readMalformed = [&](std::uint32_t address)
    { return malformed[(address - kBase) / 4]; };
    const auto rejected = gears::InspectRhiShaderLoadRange(kBase - 4, kBase + 8, readMalformed);
    assert(!rejected.complete);
    assert(rejected.packetCount == 1);
    assert(rejected.dwordCount == 3);
    assert(rejected.loads.empty());

    constexpr std::array<std::uint32_t, 1> noLoads{0x80000000};
    const auto readNoLoads = [&](std::uint32_t) { return noLoads[0]; };
    const auto zero = gears::InspectRhiShaderLoadRange(kBase - 4, kBase, readNoLoads);
    assert(zero.complete);
    assert(zero.packetCount == 1);
    assert(zero.dwordCount == 1);
    assert(zero.loads.empty());
}

void TestCommandBufferTransitions()
{
    using gears::titles::gears1::ShaderFlushRangeCapture;

    ShaderFlushRangeCapture capture;
    assert(!capture.Active());
    assert(capture.Begin(0x40001000, 0x1000));
    assert(capture.Active());
    assert(!capture.Begin(0x40001000, 0x1000));
    capture.ObserveCommandBufferTransition(0x40001000, 0x100C, 0x2000);
    const auto result = capture.Finish(0x40001000, 0x2008);
    assert(!capture.Active());
    assert(result.complete);
    assert(result.ranges.size() == 2);
    assert(result.ranges[0].commandBefore == 0x1000);
    assert(result.ranges[0].commandEnd == 0x100C);
    assert(result.ranges[1].commandBefore == 0x2000);
    assert(result.ranges[1].commandEnd == 0x2008);

    assert(capture.Begin(0x40001000, 0x3000));
    const auto empty = capture.Finish(0x40001000, 0x3000);
    assert(empty.complete);
    assert(empty.ranges.empty());

    assert(capture.Begin(0x40001000, 0x4000));
    capture.ObserveCommandBufferTransition(0x40002000, 0x4008, 0x5000);
    assert(!capture.Finish(0x40001000, 0x4008).complete);
}

} // namespace

int main()
{
    TestExactShaderLoads();
    TestNegativeAndZeroEvidence();
    TestCommandBufferTransitions();
    return 0;
}
