// The per-draw arena. gpu_draw_arena.h says what it is for; this is the
// sizing, the suballocation and the fallback a frame takes when it outgrows it.

#include "gpu_draw_arena.h"

#include <algorithm>
#include <cstring>

#include <lucent/log.h>

namespace gears::draw
{

bool FrameArena::Build(uint32_t nDraws)
{
    // Size the arena to the previous frame's high-water mark before the frame
    // starts, so the common case never allocates. The first frame has no mark
    // to go on and estimates from the draw count; if the estimate is short the
    // overflow path covers the rest and the real mark sizes the next frame.
    if (P.arenaHighWater == 0)
        P.arenaHighWater = VkDeviceSize(nDraws) * 16384;
    if (P.arenaHighWater <= F.arenaBytes)
        return true;
    if (F.arenaMapped)
        vkUnmapMemory(R.device, F.arenaMemory);
    vkDestroyBuffer(R.device, F.arena, nullptr);
    vkFreeMemory(R.device, F.arenaMemory, nullptr);
    F.arena = VK_NULL_HANDLE;
    F.arenaMemory = VK_NULL_HANDLE;
    F.arenaMapped = nullptr;
    const VkDeviceSize want = P.arenaHighWater + P.arenaHighWater / 4 + 0x10000;
    if (R.MakeBuffer(want, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     F.arena, F.arenaMemory))
    {
        F.arenaBytes = want;
        VK_CHECK(vkMapMemory(R.device, F.arenaMemory, 0, want, 0, &F.arenaMapped));
        lucent::info("draw", "per-draw arena grown to {} KiB", want / 1024);
    }
    else
    {
        F.arenaBytes = 0;
    }
    return true;
}

bool FrameArena::Write(const void *data, size_t size, VkDeviceSize alignment,
                       VkDeviceSize &outOffset)
{
    if (!F.arenaMapped)
        return false;
    const VkDeviceSize offset = (cursor + alignment - 1) & ~(alignment - 1);
    if (offset + size > F.arenaBytes)
    {
        // Remember what was asked for, so next frame's arena can actually hold it.
        wanted += (size + alignment - 1) & ~(alignment - 1);
        return false;
    }
    std::memcpy(static_cast<uint8_t *>(F.arenaMapped) + offset, data, size);
    cursor = offset + size;
    outOffset = offset;
    return true;
}

bool FrameArena::MakeIndexBuffer(const void *data, size_t bytes, VkBuffer &outBuf,
                                 VkDeviceSize &outOffset)
{
    if (Write(data, bytes, 4, outOffset))
    {
        outBuf = F.arena;
        return true;
    }
    ++overflows;
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (!R.MakeBuffer(bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, outBuf, m))
        return false;
    void *p = nullptr;
    VK_CHECK(vkMapMemory(R.device, m, 0, bytes, 0, &p));
    std::memcpy(p, data, bytes);
    vkUnmapMemory(R.device, m);
    keepBuffers.push_back(outBuf);
    keepMem.push_back(m);
    outOffset = 0;
    return true;
}

bool FrameArena::MakeUbo(const void *data, size_t size, VkDescriptorBufferInfo &out)
{
    const size_t bytes = std::max<size_t>(size, 16);
    VkDeviceSize offset = 0;
    if (Write(data, bytes, R.uniformOffsetAlignment, offset))
    {
        out = {F.arena, offset, bytes};
        return true;
    }
    ++overflows;
    VkBuffer b = VK_NULL_HANDLE;
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (!R.MakeBuffer(bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, b, m))
        return false;
    void *p = nullptr;
    VK_CHECK(vkMapMemory(R.device, m, 0, bytes, 0, &p));
    std::memcpy(p, data, size);
    vkUnmapMemory(R.device, m);
    keepBuffers.push_back(b);
    keepMem.push_back(m);
    out = {b, 0, bytes};
    return true;
}

void FrameArena::EndFrame()
{
    // What this frame NEEDED, not what it managed to fit.
    P.arenaHighWater = std::max(P.arenaHighWater, cursor + wanted);
    if (overflows)
        // Says what it will grow TO, rather than promising a fit. The old wording --
        // "next frame will fit" -- was printed every frame for thousands of frames
        // while nothing grew, because the size it would have grown to was measured
        // from what fit rather than from what was asked for.
        lucent::info("draw",
                     "per-draw arena overflowed on {} allocation(s):"
                     " {} KiB fitted, {} KiB more was wanted, arena is {} KiB and will be"
                     " sized to {} KiB",
                     overflows, cursor / 1024, wanted / 1024, F.arenaBytes / 1024,
                     (P.arenaHighWater + P.arenaHighWater / 4 + 0x10000) / 1024);
}

void FrameArena::Release()
{
    for (size_t i = 0; i < keepBuffers.size(); ++i)
    {
        vkDestroyBuffer(R.device, keepBuffers[i], nullptr);
        vkFreeMemory(R.device, keepMem[i], nullptr);
    }
    keepBuffers.clear();
    keepMem.clear();
}

} // namespace gears::draw
