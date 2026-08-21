#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "gpu_draw.h"

namespace gears::draw
{

struct ResolveConsumerExtents
{
    // A destination with one declared 2D texture extent can use that logical
    // extent for its sampled host image. The resolve pitch remains separate.
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> unique;

    // Multiple extents mean the guest aliases one destination through
    // different texture descriptions. One Vulkan image extent cannot model
    // that, so the caller keeps the pitch-sized image and reports the conflict.
    std::map<uint32_t, std::set<std::pair<uint32_t, uint32_t>>> conflicts;
};

// Finds the logical 2D texture dimensions used to sample this frame's resolve
// destinations. RB_COPY_DEST_PITCH is row stride, not sampled width: UE3's
// 322x182 bloom texture has pitch 352, and using 352 as the Vulkan image width
// changes every normalized sample coordinate.
ResolveConsumerExtents FindResolveConsumerExtents(const FrameDrawInputs &inputs,
                                                  const std::set<uint32_t> &resolveDestinations);

// Dump names retain guest pitch for structural pairing. Append the logical
// sampled extent only when it differs, so the payload geometry is unambiguous.
std::string ResolveSampleExtentSuffix(uint32_t sampledWidth, uint32_t sampledHeight,
                                      uint32_t guestPitch, uint32_t guestHeight);

} // namespace gears::draw
