#include "titles/gears1/rhi_vertex_buffer.h"

#include <cassert>

int main()
{
    const auto view = gears::gears1::DecodeVertexBufferView(0xA0100000, 0x3000, 0x40, 20);
    assert(view.has_value());
    assert(view->allocation.guestAddress == 0x00100040);
    assert(view->allocation.sizeBytes == 0x2FC0);
    assert(view->elementStrideBytes == 20);
    assert(view->endianSwap == 0);

    const auto localView = gears::gears1::DecodeVertexBufferView(0xE0100000, 0x200, 0x20, 12);
    assert(localView.has_value());
    assert(localView->allocation.guestAddress == 0x00101020);
    assert(localView->allocation.sizeBytes == 0x1E0);
    assert(gears::gears1::EncodeVertexFetchAddress(0xE0001000) == 0x00002000);

    assert(!gears::gears1::DecodeVertexBufferView(0xA0100000, 0x20, 0x21, 4).has_value());
    assert(!gears::gears1::DecodeVertexBufferView(0xFFFFFFF0, 0x40, 0x20, 4).has_value());

    const gears::RhiSemanticBufferView state =
        gears::gears1::DecodeVertexBufferState(0x00100040, 0x2FC0, 5);
    assert(state.allocation.guestAddress == view->allocation.guestAddress);
    assert(state.allocation.sizeBytes == view->allocation.sizeBytes);
    assert(state.elementStrideBytes == view->elementStrideBytes);

    return 0;
}
