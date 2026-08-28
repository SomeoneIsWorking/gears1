#include "rhi_resolve.h"

#include "gpu_draw_formats.h"
#include "guest_memory.h"
#include "guest_state_memory.h"
#include "import_stub.h"
#include "rhi_packet_evidence.h"
#include "rhi_semantic_stream.h"

#include <array>
#include <cstdint>

namespace
{

constexpr std::uint32_t kCommandWritePointerOffset = 0x28;
constexpr std::uint32_t kColorTargetObjectTableOffset = 0x2F88;
constexpr std::uint32_t kDepthTargetObjectOffset = 0x2F98;
constexpr std::uint32_t kCopyDestinationBaseShadowOffset = 0x299C;
constexpr std::uint32_t kCopyDestinationPitchHeightShadowOffset = 0x29A0;
constexpr std::uint32_t kDestinationDescriptorOffset = 0x1C;
constexpr std::uint32_t kMaximumResolvePacketSearchDwords = 4096;

[[nodiscard]] std::uint32_t SourceObject(gears::titles::gears1::GuestStateMemory &memory,
                                         std::uint32_t device, std::uint32_t flags)
{
    const std::uint32_t selection = flags & 7;
    if (device == 0 || selection > 4)
        return 0;
    if (selection == 4)
        return memory.Read32(device + kDepthTargetObjectOffset);
    return memory.Read32(device + kColorTargetObjectTableOffset +
                         selection * sizeof(std::uint32_t));
}

[[nodiscard]] std::array<std::int32_t, 4>
ReadSourceRectangle(gears::titles::gears1::GuestStateMemory &memory, std::uint32_t address)
{
    std::array<std::int32_t, 4> rectangle{};
    if (address == 0)
        return rectangle;
    for (std::uint32_t index = 0; index < rectangle.size(); ++index)
    {
        rectangle[index] =
            static_cast<std::int32_t>(memory.Read32(address + index * sizeof(std::uint32_t)));
    }
    return rectangle;
}

[[nodiscard]] std::array<std::int32_t, 2>
ReadDestinationPoint(gears::titles::gears1::GuestStateMemory &memory, std::uint32_t address)
{
    std::array<std::int32_t, 2> point{};
    if (address == 0)
        return point;
    for (std::uint32_t index = 0; index < point.size(); ++index)
        point[index] =
            static_cast<std::int32_t>(memory.Read32(address + index * sizeof(std::uint32_t)));
    return point;
}

[[nodiscard]] std::array<std::uint32_t, 6>
ReadDestinationDescriptor(gears::titles::gears1::GuestStateMemory &memory, std::uint32_t object)
{
    std::array<std::uint32_t, 6> descriptor{};
    if (object == 0)
        return descriptor;
    for (std::uint32_t index = 0; index < descriptor.size(); ++index)
    {
        descriptor[index] =
            memory.Read32(object + kDestinationDescriptorOffset + index * sizeof(std::uint32_t));
    }
    return descriptor;
}

} // namespace

extern "C" PPC_FUNC(__imp__sub_82235528);
PPC_FUNC(sub_82235528)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_82235528(ctx, base);
        return;
    }

    gears::titles::gears1::GuestStateMemory memory(base);
    const std::uint32_t device = ctx.r3.u32;
    const std::uint32_t flags = ctx.r4.u32;
    const std::uint32_t destinationObject = ctx.r6.u32;
    const std::array<std::uint32_t, 6> destinationDescriptor =
        ReadDestinationDescriptor(memory, destinationObject);
    const gears::gears1::ResolveCallState call{
        .flags = flags,
        .sourceObject = SourceObject(memory, device, flags),
        .destinationObject = destinationObject,
        .destinationDescriptor = destinationDescriptor,
        .sourceRectangle = ReadSourceRectangle(memory, ctx.r5.u32),
        .destinationPoint = ReadDestinationPoint(memory, ctx.r7.u32),
        .bytesPerBlock = gears::draw::ColorFormatBytesPerPixel(destinationDescriptor[1] & 0x3F),
    };
    const gears::RhiSemanticResolve resolve = gears::gears1::DecodeResolveCall(call);
    const std::uint32_t commandBefore =
        device != 0 ? memory.Read32(device + kCommandWritePointerOffset) : 0;

    __imp__sub_82235528(ctx, base);

    const std::uint32_t commandEnd =
        device != 0 ? memory.Read32(device + kCommandWritePointerOffset) : 0;
    const gears::RhiBasicDrawPacketEvidence draw = gears::FindLastRhiDrawPacketAcrossCommandBuffers(
        commandBefore, commandEnd, kMaximumResolvePacketSearchDwords,
        [&memory](std::uint32_t address) { return memory.Read32(address); });
    const std::uint32_t pitchHeight =
        device != 0 ? memory.Read32(device + kCopyDestinationPitchHeightShadowOffset) : 0;
    gears::ObserveRhiSemanticResolve(
        resolve, {
                     .present = draw.present,
                     .observedSourceObject = SourceObject(memory, device, flags),
                     .destinationAddress =
                         device != 0 ? memory.Read32(device + kCopyDestinationBaseShadowOffset) &
                                           gears::GuestMemory::kAliasMask
                                     : 0,
                     .destinationPitch = pitchHeight & 0x3FFF,
                     .destinationHeight = (pitchHeight >> 16) & 0x3FFF,
                     .drawOpcode = draw.opcode,
                     .primitiveType = draw.primitiveType,
                     .sourceSelect = draw.sourceSelect,
                     .elementCount = draw.elementCount,
                 });
}
