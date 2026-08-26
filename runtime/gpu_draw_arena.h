#pragma once

// The per-draw arena: one persistently-mapped buffer that every draw's uniform
// blocks and expanded index buffers are suballocated from, instead of a
// VkBuffer created, mapped, copied and unmapped per block.
//
// It is sized from the PREVIOUS frame's high-water mark, and the mark is what
// the frame NEEDED rather than what it managed to fit -- see msWanted in
// EndFrame for why that distinction is the whole mechanism, and what it cost
// when it was missing.

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "gpu_draw_renderer.h"

namespace gears::draw
{

struct FrameArena
{
    FrameArena(Renderer &r, RendererPersistent &p, GpuFrameResources &frame) : R(r), P(p), F(frame)
    {
    }

    // Sizes (and if necessary regrows) the arena for a frame of nDraws draws.
    // False means the buffer could not be created or mapped; there is no
    // drawing without it, so the caller must abandon the frame rather than
    // fall back per allocation and call the result a slow frame.
    bool Build(uint32_t nDraws);

    Renderer &R;
    RendererPersistent &P;
    GpuFrameResources &F;

    VkDeviceSize cursor = 0;
    uint32_t overflows = 0;
    // Bytes that did NOT fit. This is the number the next frame's arena has to be
    // sized from, and its absence was a self-perpetuating bug: the high-water mark
    // was taken from the cursor, which only advances for allocations that FIT, so it
    // could never exceed the current arena size -- and the growth test
    // (arenaHighWater > arenaBytes) could therefore never be true. The arena stayed at
    // its first size forever while 2618 blocks a frame took the fallback that creates,
    // maps, copies and unmaps a VkBuffer each, which is where 104 of a 170 ms frame
    // went. The log even said "next frame will fit" every frame, and it never did.
    VkDeviceSize wanted = 0;

    // Per-draw resources the fallback path created, kept alive until after the
    // submit completes. Only a frame that outgrows the arena fills these.
    std::vector<VkBuffer> keepBuffers;
    std::vector<VkDeviceMemory> keepMem;

    // Reused by index conversion for every draw in this frame. Keeping the
    // capacity here removes two heap allocations per indexed draw without
    // changing the authoritative conversion path.
    std::vector<uint32_t> indexSource;
    std::vector<uint32_t> indexExpanded;

    // Copies `size` bytes into the arena and reports where they landed, or
    // returns false when the arena is full (the caller then falls back).
    bool Write(const void *data, size_t size, VkDeviceSize alignment, VkDeviceSize &outOffset);

    // An index buffer for one draw, from the same arena. Index offsets need
    // 4-byte alignment; the buffer is bound with that offset.
    bool MakeIndexBuffer(const void *data, size_t bytes, VkBuffer &outBuf, VkDeviceSize &outOffset);

    // A uniform block for one draw: (buffer, offset, range) rather than a whole
    // VkBuffer of its own.
    bool MakeUbo(const void *data, size_t size, VkDescriptorBufferInfo &out);

    // Records what this frame needed so the next one is sized for it, and
    // reports any overflow. Call once the last allocation is made.
    void EndFrame();

    // Destroys the fallback buffers. Call only after the frame's fence has been
    // waited on -- recorded draws still reference them until then.
    void Release();
};

} // namespace gears::draw
