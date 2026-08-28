#include "rhi_resource_identity.h"

#include "guest_memory.h"
#include "guest_state_memory.h"

namespace gears::titles::gears1
{
namespace
{

constexpr std::uint32_t kResourceFlagsOffset = 0;
constexpr std::uint32_t kReferenceCountOffset = 4;
constexpr std::uint32_t kBackingObjectOffset = 24;
constexpr std::uint32_t kResourceTypeMask = 0xF;

} // namespace

RhiResourceIdentityEvidence CaptureRhiResourceIdentity(std::uint32_t object)
{
    if (object == 0)
        return {};

    const GuestStateMemory memory(gears::Memory().Base());
    const std::uint32_t rawFlags = memory.Read32(object + kResourceFlagsOffset);
    return {.present = true,
            .object = object,
            .rawFlags = rawFlags,
            .resourceType = rawFlags & kResourceTypeMask,
            .backingObject = memory.Read32(object + kBackingObjectOffset),
            .referenceCount = memory.Read32(object + kReferenceCountOffset)};
}

} // namespace gears::titles::gears1
