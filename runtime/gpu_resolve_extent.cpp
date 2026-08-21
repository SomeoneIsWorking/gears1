#include "gpu_resolve_extent.h"

#include <format>

namespace gears::draw
{

ResolveConsumerExtents FindResolveConsumerExtents(const FrameDrawInputs &inputs,
                                                  const std::set<uint32_t> &resolveDestinations)
{
    std::map<uint32_t, std::set<std::pair<uint32_t, uint32_t>>> found;
    for (const FrameDrawItem &draw : inputs.draws)
    {
        const uint32_t *registers = draw.registers();
        if (!registers)
            continue;
        for (uint32_t fetchIndex = 0; fetchIndex < 32; ++fetchIndex)
        {
            const uint32_t *fetch = registers + 0x4800 + fetchIndex * 6;
            if ((fetch[0] & 3u) != 2u) // FetchConstantType::kTexture
                continue;
            const uint32_t base = fetch[1] & 0xFFFFF000u;
            if (!resolveDestinations.contains(base))
                continue;
            const uint32_t dimension = (fetch[5] >> 9) & 3u;
            if (dimension != 1u) // DataDimension::k2DOrStacked
                continue;
            const uint32_t width = (fetch[2] & 0x1FFFu) + 1;
            const uint32_t height = ((fetch[2] >> 13) & 0x1FFFu) + 1;
            found[base].insert({width, height});
        }
    }

    ResolveConsumerExtents result;
    for (auto &[base, extents] : found)
    {
        if (extents.size() == 1)
            result.unique.emplace(base, *extents.begin());
        else
            result.conflicts.emplace(base, std::move(extents));
    }
    return result;
}

std::string ResolveSampleExtentSuffix(uint32_t sampledWidth, uint32_t sampledHeight,
                                      uint32_t guestPitch, uint32_t guestHeight)
{
    return sampledWidth == guestPitch && sampledHeight == guestHeight
               ? std::string{}
               : std::format("_sample{}x{}", sampledWidth, sampledHeight);
}

} // namespace gears::draw
