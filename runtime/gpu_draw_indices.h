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
    kOk,           // buffer is set, or the draw needs none (kAutoIndex)
    kEmptyQuad,    // a quad list of fewer than 4 vertices: nothing to draw
    kArenaFull,    // the index bytes did not fit and no fallback buffer could be made
};

// A kAutoIndex draw needs no buffer: gl_VertexIndex = 0..count-1 is fed
// directly (vkCmdDraw), matching how the hardware sequences an auto-indexed
// primitive, and the shader's vfetch reads the vertex from the SSBO by that
// index. Such a draw returns kOk with buffer left null.
IndexResult PrepareIndices(FrameArena& arena, const FrameDrawInputs& in,
                           const FrameDrawItem& d, PreparedIndices& out);

} // namespace gears::draw
