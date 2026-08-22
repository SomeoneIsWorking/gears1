#pragma once

// One draw's index buffer: read the guest's indices, byte-swap them, widen
// them, and expand the topologies Vulkan does not have.
//
// Two things here are not conveniences, they are the difference between a
// frame and nothing:
//
//   kQuadList (0x0D) has no Vulkan topology. The hardware draws each group of
//   4 vertices as a quad; this expands to a triangle list (0,1,2 / 0,2,3)
//   rather than pretending a quad list is a triangle list. Without it the
//   vertices regroup into unrelated triangles -- and an Act 1 frame's ENTIRE
//   world geometry is quad_list, so it drew nothing.
//
//   The buffer is ALWAYS 32-bit. The draw binds VK_INDEX_TYPE_UINT32
//   unconditionally, so sizing it by the guest's index width made every 16-bit
//   indexed draw read twice its buffer (validation
//   VUID-vkCmdDrawIndexed-robustBufferAccess2-08798) and rasterise garbage.

#include <cstdint>
#include <unordered_map>

#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "gpu_draw_arena.h"

namespace gears::draw
{

struct PreparedIndices
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    // What the draw call should use, which is NOT always what the guest asked
    // for: expanding a quad list changes both the count and whether the draw
    // is indexed at all.
    uint32_t count = 0;
    bool indexed = false;
};

enum class IndexResult
{
    kOk,        // buffer is set, or the draw needs none (kAutoIndex)
    kEmptyQuad, // a quad list of fewer than 4 vertices: nothing to draw
    kArenaFull, // the index bytes did not fit and no fallback buffer could be made
};

// One frame's exact index-conversion reuse table. Predicated EDRAM tiling can
// submit the same guest draw for several tiles; conversion happens before the
// tile groups are collapsed, so without this table every copy byte-swaps,
// widens and uploads identical indices into another arena range.
class IndexReuseTable
{
  public:
    bool Find(const FrameDrawItem &draw, PreparedIndices &out);
    void Store(const FrameDrawItem &draw, const PreparedIndices &indices);
    size_t Size() const { return entries_.size(); }

  private:
    struct Key
    {
        uint32_t guestBase = 0;
        uint32_t count = 0;
        uint32_t endian = 0;
        uint32_t primitive = 0;
        bool indexed = false;
        bool is32 = false;

        bool operator==(const Key &) const = default;
    };

    struct Hash
    {
        size_t operator()(const Key &key) const;
    };

    static Key MakeKey(const FrameDrawItem &draw);
    std::unordered_map<Key, PreparedIndices, Hash> entries_;
};

class IndexPreparer
{
  public:
    uint64_t lookups = 0;
    uint64_t hits = 0;
    uint64_t builds = 0;

    // A kAutoIndex draw needs no buffer: gl_VertexIndex = 0..count-1 is fed
    // directly, matching the hardware sequence. Quad lists and DMA indices go
    // through the exact conversion table above.
    IndexResult Prepare(FrameArena &arena, const FrameDrawInputs &in, const FrameDrawItem &draw,
                        PreparedIndices &out);
    size_t CacheSize() const { return reuse_.Size(); }
    void Report() const;

  private:
    IndexReuseTable reuse_;
};

} // namespace gears::draw
