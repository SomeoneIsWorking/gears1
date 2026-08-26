// One draw's index buffer. gpu_draw_indices.h says why the two transforms
// here are not optional.

#include "gpu_draw_indices.h"

#include "gpu_endian.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <lucent/log.h>

namespace gears::draw
{

namespace
{

// Both answer classes are compile-time gates in the shipping implementation,
// not a helper beside it. Uniform output here would corrupt mode 0 or 3 while
// still producing perfectly plausible draw and primitive counts.
static_assert(SwapGpuIndex16(0x1234u, 0) == 0x1234u);
static_assert(SwapGpuIndex16(0x1234u, 1) == 0x3412u);
static_assert(SwapGpuIndex16(0x1234u, 2) == 0x3412u);
static_assert(SwapGpuIndex16(0x1234u, 3) == 0x1234u);
static_assert(SwapGpuWord32(0x12345678u, 0) == 0x12345678u);
static_assert(SwapGpuWord32(0x12345678u, 1) == 0x34127856u);
static_assert(SwapGpuWord32(0x12345678u, 2) == 0x78563412u);
static_assert(SwapGpuWord32(0x12345678u, 3) == 0x56781234u);

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
            v = SwapGpuWord32(v, d.indexEndian);
        }
        else if (inRange)
        {
            uint16_t h = 0;
            std::memcpy(&h, base + i * 2, 2);
            v = SwapGpuIndex16(h, d.indexEndian);
        }
        dst[i] = v;
    }
}

} // namespace

static IndexResult ConvertIndices(FrameArena &arena, const FrameDrawInputs &in,
                                  const FrameDrawItem &d, PreparedIndices &out)
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

IndexReuseTable::Key IndexReuseTable::MakeKey(const FrameDrawItem &draw)
{
    return {draw.indexGuestBase, draw.indexCount, draw.indexEndian,
            draw.primType,       draw.indexed,    draw.indexIs32};
}

size_t IndexReuseTable::Hash::operator()(const Key &key) const
{
    size_t hash = key.guestBase;
    auto mix = [&hash](uint32_t value)
    { hash ^= size_t(value) + 0x9E3779B9u + (hash << 6) + (hash >> 2); };
    mix(key.count);
    mix(key.endian);
    mix(key.primitive);
    mix(key.indexed);
    mix(key.is32);
    return hash;
}

bool IndexReuseTable::Find(const FrameDrawItem &draw, PreparedIndices &out)
{
    const auto found = entries_.find(MakeKey(draw));
    if (found == entries_.end())
        return false;
    out = found->second;
    return true;
}

void IndexReuseTable::Store(const FrameDrawItem &draw, const PreparedIndices &indices)
{
    entries_.emplace(MakeKey(draw), indices);
}

IndexResult IndexPreparer::Prepare(FrameArena &arena, const FrameDrawInputs &in,
                                   const FrameDrawItem &draw, PreparedIndices &out)
{
    const bool cacheable = draw.indexed || draw.primType == 13 /*kQuadList*/;
    if (!cacheable)
        return ConvertIndices(arena, in, draw, out);

    ++lookups;
    if (reuse_.Find(draw, out))
    {
        ++hits;
        return IndexResult::kOk;
    }

    ++builds;
    const IndexResult result = ConvertIndices(arena, in, draw, out);
    if (result == IndexResult::kOk)
        reuse_.Store(draw, out);
    return result;
}

void IndexPreparer::Report() const
{
    lucent::info("draw",
                 "index conversion: {} of {} reusable draw(s) hit an exact"
                 " frame-local entry; {} built, {} distinct entries",
                 hits, lookups, builds, CacheSize());
}

} // namespace gears::draw
