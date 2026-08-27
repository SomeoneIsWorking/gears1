#include "rhi_resolve.h"

#include "guest_address.h"

#include <cstdint>
#include <limits>

namespace gears::gears1
{

RhiSemanticResolve DecodeResolveCall(const ResolveCallState &call)
{
    const std::uint32_t sourceSelection = call.flags & 7;
    RhiSemanticResolve resolve{
        .sourceDepthStencil = sourceSelection == 4,
        .sourceSlot = sourceSelection == 4 ? 0u : sourceSelection,
        .sourceObject = call.sourceObject,
        .destinationObject = call.destinationObject,
        .destinationFormat = call.destinationDescriptor[1] & 0x3F,
    };
    if (sourceSelection > 4 || call.destinationObject == 0 || call.bytesPerBlock == 0)
        return resolve;

    const std::uint32_t descriptorPitch = (call.destinationDescriptor[0] >> 17) & 0x3FE0;
    const std::uint32_t dimension = (call.destinationDescriptor[5] >> 9) & 3;
    const std::uint32_t heightMask = dimension == 2 ? 0x7FFu : 0x1FFFu;
    const std::uint32_t heightShift = dimension == 2 ? 11u : 13u;
    const std::uint32_t extentIncrement = ((call.destinationDescriptor[3] >> 30) & 2) + 1;
    const std::uint32_t fullHeight =
        ((call.destinationDescriptor[2] >> heightShift) & heightMask) + extentIncrement;

    const std::int64_t sourceX = call.sourceRectangle[0];
    const std::int64_t sourceY = call.sourceRectangle[1];
    const std::int64_t destinationX = call.destinationPoint[0];
    const std::int64_t destinationY = call.destinationPoint[1];
    const std::int64_t destinationOffsetBlocks =
        (destinationY - sourceY) * descriptorPitch + (destinationX - sourceX) * 32;
    if (descriptorPitch == 0 || destinationY < 0 ||
        destinationY > static_cast<std::int64_t>(fullHeight) || destinationOffsetBlocks < 0)
    {
        return resolve;
    }

    const std::uint64_t destinationOffsetBytes =
        static_cast<std::uint64_t>(destinationOffsetBlocks) * call.bytesPerBlock;
    if (destinationOffsetBytes > std::numeric_limits<std::uint32_t>::max())
        return resolve;

    const std::uint32_t baseAddress =
        call.destinationDescriptor[1] & 0xFFFFF000u & kGuestPhysicalAddressMask;
    resolve.destinationAddress =
        (baseAddress + static_cast<std::uint32_t>(destinationOffsetBytes)) &
        kGuestPhysicalAddressMask;
    resolve.destinationPitch = descriptorPitch;
    resolve.destinationHeight = fullHeight - static_cast<std::uint32_t>(destinationY);
    return resolve;
}

} // namespace gears::gears1
