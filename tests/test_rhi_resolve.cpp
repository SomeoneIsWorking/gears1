#include "gpu_draw_formats.h"
#include "rhi_packet_evidence.h"
#include "titles/gears1/rhi_resolve.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <map>

namespace
{

gears::gears1::ResolveCallState MakeResolveCall()
{
    gears::gears1::ResolveCallState call{
        .flags = 0,
        .sourceObject = 0x40102000,
        .destinationObject = 0x40104000,
        .sourceRectangle = {0, 0, 1280, 208},
        .destinationPoint = {0, 512},
        .bytesPerBlock = 4,
    };
    call.destinationDescriptor[0] = 1280u << 17;
    call.destinationDescriptor[1] = 0x0BA40000u | 23u;
    call.destinationDescriptor[2] = 719u << 13;
    return call;
}

} // namespace

int main()
{
    assert(gears::draw::ColorFormatBytesPerPixel(27) == 2);
    assert(gears::draw::ColorFormatBytesPerPixel(28) == 4);
    assert(gears::draw::ColorFormatBytesPerPixel(29) == 8);

    gears::gears1::ResolveCallState call = MakeResolveCall();
    const gears::RhiSemanticResolve resolve = gears::gears1::DecodeResolveCall(call);
    assert(!resolve.sourceDepthStencil);
    assert(resolve.sourceSlot == 0);
    assert(resolve.sourceObject == call.sourceObject);
    assert(resolve.destinationObject == call.destinationObject);
    assert(resolve.destinationAddress == 0x0BCC0000);
    assert(resolve.destinationPitch == 1280);
    assert(resolve.destinationHeight == 208);
    assert(resolve.destinationFormat == 23);
    assert(resolve.operationFlags == call.flags);
    assert(resolve.sourceRectangle == call.sourceRectangle);
    assert(resolve.destinationPoint == call.destinationPoint);
    assert(resolve.destinationDescriptor == call.destinationDescriptor);
    assert(resolve.bytesPerBlock == call.bytesPerBlock);

    call.flags = 4;
    const gears::RhiSemanticResolve depth = gears::gears1::DecodeResolveCall(call);
    assert(depth.sourceDepthStencil);
    assert(depth.sourceSlot == 0);

    call = MakeResolveCall();
    call.destinationPoint[1] = -1;
    const gears::RhiSemanticResolve invalidPoint = gears::gears1::DecodeResolveCall(call);
    assert(invalidPoint.destinationAddress == 0);
    assert(invalidPoint.destinationPitch == 0);

    call = MakeResolveCall();
    call.flags = 5;
    const gears::RhiSemanticResolve invalidSource = gears::gears1::DecodeResolveCall(call);
    assert(invalidSource.destinationAddress == 0);

    constexpr std::uint32_t kHeaderAddress = 0x1000;
    constexpr std::uint32_t kCommandEnd = kHeaderAddress + 2 * sizeof(std::uint32_t);
    std::map<std::uint32_t, std::uint32_t> words{
        {kHeaderAddress, 0xC0012200},
        {kHeaderAddress + sizeof(std::uint32_t), 0},
        {kCommandEnd, 0x00030088},
    };
    const auto readWord = [&words](std::uint32_t address) { return words[address]; };
    const gears::RhiBasicDrawPacketEvidence draw = gears::FindLastRhiDrawPacket(
        kCommandEnd, kHeaderAddress - sizeof(std::uint32_t), 8, readWord);
    assert(draw.present);
    assert(draw.opcode == 0x22);
    assert(draw.primitiveType == 8);
    assert(draw.sourceSelect == 2);
    assert(draw.elementCount == 3);
    assert(draw.headerAddress == kHeaderAddress);

    const gears::RhiBasicDrawPacketEvidence outsideSpan =
        gears::FindLastRhiDrawPacket(kCommandEnd, kHeaderAddress, 8, readWord);
    assert(!outsideSpan.present);

    return 0;
}
