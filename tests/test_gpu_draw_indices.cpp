#include "gpu_draw_indices.h"

#include <cassert>

namespace
{

gears::FrameDrawItem BaseDraw()
{
    gears::FrameDrawItem draw;
    draw.indexGuestBase = 0x123400;
    draw.indexCount = 96;
    draw.indexEndian = 2;
    draw.primType = 13;
    draw.indexed = true;
    draw.indexIs32 = false;
    return draw;
}

} // namespace

int main()
{
    gears::draw::IndexReuseTable table;
    gears::draw::PreparedIndices stored;
    stored.offset = 64;
    stored.count = 144;
    stored.indexed = true;

    gears::FrameDrawItem draw = BaseDraw();
    table.Store(draw, stored);

    gears::draw::PreparedIndices found;
    assert(table.Find(draw, found));
    assert(found.buffer == stored.buffer);
    assert(found.offset == stored.offset);
    assert(found.count == stored.count);
    assert(found.indexed == stored.indexed);

    auto mustMiss = [&](auto change)
    {
        gears::FrameDrawItem different = BaseDraw();
        change(different);
        assert(!table.Find(different, found));
    };
    mustMiss([](auto &d) { ++d.indexGuestBase; });
    mustMiss([](auto &d) { ++d.indexCount; });
    mustMiss([](auto &d) { ++d.indexEndian; });
    mustMiss([](auto &d) { ++d.primType; });
    mustMiss([](auto &d) { d.indexed = false; });
    mustMiss([](auto &d) { d.indexIs32 = true; });
    assert(table.Size() == 1);
    return 0;
}
