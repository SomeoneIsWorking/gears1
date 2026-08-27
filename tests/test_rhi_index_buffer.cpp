#include "titles/gears1/rhi_index_buffer.h"

#include <cassert>

int main()
{
    const gears::RhiSemanticBufferView index16 =
        gears::gears1::DecodeIndexBufferView(0x20000002, 0xAA08D800, 0x1518);
    assert(index16.allocation.guestAddress == 0x0A08D800);
    assert(index16.allocation.sizeBytes == 0x1518);
    assert(index16.elementStrideBytes == 2);
    assert(index16.endianSwap == 1);

    const auto slice16 = gears::gears1::IndexBufferSlice(index16, 7, 18);
    assert(slice16.has_value());
    assert(slice16->guestAddress == 0x0A08D80E);
    assert(slice16->sizeBytes == 36);
    assert(!gears::gears1::IndexBufferSlice(index16, 2700, 1).has_value());

    const gears::RhiSemanticBufferView index32 =
        gears::gears1::DecodeIndexBufferView(0xC0000002, 0xA0BE7E40, 0x120);
    assert(index32.allocation.guestAddress == 0x00BE7E40);
    assert(index32.allocation.sizeBytes == 0x120);
    assert(index32.elementStrideBytes == 4);
    assert(index32.endianSwap == 2);

    const auto slice32 = gears::gears1::IndexBufferSlice(index32, 3, 6);
    assert(slice32.has_value());
    assert(slice32->guestAddress == 0x00BE7E4C);
    assert(slice32->sizeBytes == 24);

    return 0;
}
