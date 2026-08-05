// One draw's index buffer. gpu_draw_indices.h says why the two transforms
// here are not optional.

#include "gpu_draw_indices.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace gears::draw
{

namespace
{

// The guest's indices as 32-bit host values. A fetch that would read past the
// guest-memory mirror yields ZERO rather than reading out of bounds -- the
// draw then collapses at clipping, which the frame report counts as geometry
// reach rather than letting it look like a shading defect.
void ReadGuestIndices(const FrameDrawInputs& in, const FrameDrawItem& d,
                      uint32_t* dst, uint32_t count)
{
    const uint8_t* base = in.guestBase + d.indexGuestBase;
    const uint32_t width = d.indexIs32 ? 4u : 2u;
    const bool inRange =
        d.indexGuestBase + uint64_t(count) * width <= in.guestPhysicalMirrorBytes;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t v = 0;
        if (inRange && d.indexIs32)
        { std::memcpy(&v, base + i * 4, 4); v = __builtin_bswap32(v); }
        else if (inRange)
        {
            uint16_t h = 0; std::memcpy(&h, base + i * 2, 2);
            v = uint16_t((h >> 8) | (h << 8));
        }
        dst[i] = v;
    }
}

} // namespace

IndexResult PrepareIndices(FrameArena& arena, const FrameDrawInputs& in,
                           const FrameDrawItem& d, PreparedIndices& out)
{
    out = PreparedIndices{};
    out.count = d.indexCount;
    out.indexed = d.indexed;

    if (d.primType == 13 /*kQuadList*/)
    {
        const uint32_t quads = d.indexCount / 4;
        const uint32_t triIndices = quads * 6;
        if (quads == 0)
            return IndexResult::kEmptyQuad;
        // Guest indices (when present) are read first, then regrouped, so the
        // expansion works for both auto and DMA quad lists.
        std::vector<uint32_t> src(d.indexCount);
        if (d.indexed)
            ReadGuestIndices(in, d, src.data(), d.indexCount);
        else
            for (uint32_t i = 0; i < d.indexCount; ++i)
                src[i] = i;
        std::vector<uint32_t> expanded(triIndices);
        uint32_t* dst = expanded.data();
        for (uint32_t q = 0; q < quads; ++q)
        {
            const uint32_t* v = &src[q * 4];
            *dst++ = v[0]; *dst++ = v[1]; *dst++ = v[2];
            *dst++ = v[0]; *dst++ = v[2]; *dst++ = v[3];
        }
        if (!arena.MakeIndexBuffer(expanded.data(), triIndices * 4u,
                                   out.buffer, out.offset))
            return IndexResult::kArenaFull;
        out.count = triIndices;
        out.indexed = true;
        return IndexResult::kOk;
    }

    if (d.indexed)
    {
        const uint32_t idxBytes = std::max(d.indexCount * 4u, 4u);
        std::vector<uint32_t> widened(idxBytes / 4, 0);
        ReadGuestIndices(in, d, widened.data(), d.indexCount);
        if (!arena.MakeIndexBuffer(widened.data(), idxBytes, out.buffer, out.offset))
            return IndexResult::kArenaFull;
    }
    return IndexResult::kOk;
}

} // namespace gears::draw
