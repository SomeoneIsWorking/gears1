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

constexpr uint16_t SwapIndex16(uint16_t value, uint32_t endian)
{
    // For 16-bit DMA, Xenia maps k8in32 to k8in16 and k16in32 to kNone.
    return endian == 1 || endian == 2 ? uint16_t((value >> 8) | (value << 8)) : value;
}

constexpr uint32_t SwapIndex32(uint32_t value, uint32_t endian)
{
    switch (endian & 3u)
    {
    case 0:
        return value;
    case 1:
        return ((value & 0x00FF00FFu) << 8) | ((value & 0xFF00FF00u) >> 8);
    case 2:
        return __builtin_bswap32(value);
    case 3:
        return (value << 16) | (value >> 16);
    }
    return value;
}

// Both answer classes are compile-time gates in the shipping implementation,
// not a helper beside it. Uniform output here would corrupt mode 0 or 3 while
// still producing perfectly plausible draw and primitive counts.
static_assert(SwapIndex16(0x1234u, 0) == 0x1234u);
static_assert(SwapIndex16(0x1234u, 1) == 0x3412u);
static_assert(SwapIndex16(0x1234u, 2) == 0x3412u);
static_assert(SwapIndex16(0x1234u, 3) == 0x1234u);
static_assert(SwapIndex32(0x12345678u, 0) == 0x12345678u);
static_assert(SwapIndex32(0x12345678u, 1) == 0x34127856u);
static_assert(SwapIndex32(0x12345678u, 2) == 0x78563412u);
static_assert(SwapIndex32(0x12345678u, 3) == 0x56781234u);

// The guest's indices as 32-bit host values. A fetch that would read past the
// guest-memory mirror yields ZERO rather than reading out of bounds -- the
// draw then collapses at clipping, which the frame report counts as geometry
// reach rather than letting it look like a shading defect.
void ReadGuestIndices(const FrameDrawInputs &in, const FrameDrawItem &d, uint32_t *dst,
                      uint32_t count)
{
    const uint8_t *base = in.guestBase + d.indexGuestBase;
    const uint32_t width = d.indexIs32 ? 4u : 2u;
    const bool inRange = d.indexGuestBase + uint64_t(count) * width <= in.guestPhysicalMirrorBytes;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t v = 0;
        if (inRange && d.indexIs32)
        {
            std::memcpy(&v, base + i * 4, 4);
            v = SwapIndex32(v, d.indexEndian);
        }
        else if (inRange)
        {
            uint16_t h = 0;
            std::memcpy(&h, base + i * 2, 2);
            v = SwapIndex16(h, d.indexEndian);
        }
        dst[i] = v;
    }
}

} // namespace

IndexResult PrepareIndices(FrameArena &arena, const FrameDrawInputs &in, const FrameDrawItem &d,
                           PreparedIndices &out)
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
        std::vector<uint32_t> &src = arena.indexSource;
        src.resize(d.indexCount);
        if (d.indexed)
            ReadGuestIndices(in, d, src.data(), d.indexCount);
        else
            for (uint32_t i = 0; i < d.indexCount; ++i)
                src[i] = i;
        std::vector<uint32_t> &expanded = arena.indexExpanded;
        expanded.resize(triIndices);
        uint32_t *dst = expanded.data();
        for (uint32_t q = 0; q < quads; ++q)
        {
            const uint32_t *v = &src[q * 4];
            *dst++ = v[0];
            *dst++ = v[1];
            *dst++ = v[2];
            *dst++ = v[0];
            *dst++ = v[2];
            *dst++ = v[3];
        }
        if (!arena.MakeIndexBuffer(expanded.data(), triIndices * 4u, out.buffer, out.offset))
            return IndexResult::kArenaFull;
        out.count = triIndices;
        out.indexed = true;
        return IndexResult::kOk;
    }

    if (d.indexed)
    {
        const uint32_t idxBytes = std::max(d.indexCount * 4u, 4u);
        std::vector<uint32_t> &widened = arena.indexSource;
        widened.resize(idxBytes / 4);
        if (d.indexCount == 0)
            widened[0] = 0;
        ReadGuestIndices(in, d, widened.data(), d.indexCount);
        if (!arena.MakeIndexBuffer(widened.data(), idxBytes, out.buffer, out.offset))
            return IndexResult::kArenaFull;
    }
    return IndexResult::kOk;
}

} // namespace gears::draw
