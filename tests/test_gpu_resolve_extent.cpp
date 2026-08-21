#include "gpu_resolve_extent.h"

#include <cassert>
#include <memory>
#include <set>
#include <vector>

namespace
{

gears::FrameDrawItem MakeDraw(uint32_t base, uint32_t pitch, uint32_t width, uint32_t height)
{
    auto registers = std::make_shared<std::vector<uint32_t>>(0x8000);
    uint32_t *fetch = registers->data() + 0x4800;
    fetch[0] = 2u | ((pitch / 32) << 22); // texture fetch and guest row pitch
    fetch[1] = base;
    fetch[2] = (width - 1) | ((height - 1) << 13);
    fetch[5] = 1u << 9; // DataDimension::k2DOrStacked
    gears::FrameDrawItem draw;
    draw.registerFile = std::move(registers);
    return draw;
}

} // namespace

int main()
{
    assert(gears::draw::ResolveSampleExtentSuffix(322, 182, 352, 182) == "_sample322x182");
    assert(gears::draw::ResolveSampleExtentSuffix(1280, 720, 1280, 720).empty());

    gears::FrameDrawInputs inputs;
    inputs.draws.push_back(MakeDraw(0x006E4000, 352, 322, 182));
    inputs.draws.push_back(MakeDraw(0x00ABC000, 1280, 1280, 720));

    auto extents =
        gears::draw::FindResolveConsumerExtents(inputs, {0x006E4000, 0x00ABC000, 0x00DEF000});
    assert(extents.unique.at(0x006E4000) == std::pair(322u, 182u));
    assert(extents.unique.at(0x00ABC000) == std::pair(1280u, 720u));
    assert(!extents.unique.contains(0x00DEF000));
    assert(extents.conflicts.empty());

    inputs.draws.push_back(MakeDraw(0x006E4000, 352, 320, 180));
    extents = gears::draw::FindResolveConsumerExtents(inputs, {0x006E4000});
    assert(!extents.unique.contains(0x006E4000));
    assert(extents.conflicts.at(0x006E4000).size() == 2);

    return 0;
}
