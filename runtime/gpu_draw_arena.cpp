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
    if (P.arenaHighWater <= P.arenaBytes)
        return true;
    if (P.arenaMapped)
        vkUnmapMemory(R.device, P.arenaMem);
    vkDestroyBuffer(R.device, P.arena, nullptr);
    vkFreeMemory(R.device, P.arenaMem, nullptr);
    P.arena = VK_NULL_HANDLE; P.arenaMem = VK_NULL_HANDLE; P.arenaMapped = nullptr;
    const VkDeviceSize want = P.arenaHighWater + P.arenaHighWater / 4 + 0x10000;
    if (R.MakeBuffer(want, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT, P.arena, P.arenaMem))
    {
        P.arenaBytes = want;
        VK_CHECK(vkMapMemory(R.device, P.arenaMem, 0, want, 0, &P.arenaMapped));
        lucent::info("draw", "per-draw arena grown to {} KiB", want / 1024);
    }
    else
    {
        P.arenaBytes = 0;
    }
    return true;
}

bool FrameArena::Write(const void* data, size_t size, VkDeviceSize alignment,
                       VkDeviceSize& outOffset)
{
    if (!P.arenaMapped)
        return false;
    const VkDeviceSize offset = (cursor + alignment - 1) & ~(alignment - 1);
    if (offset + size > P.arenaBytes)
    {
        // Remember what was asked for, so next frame's arena can actually hold it.
        wanted += (size + alignment - 1) & ~(alignment - 1);
        return false;
    }
    std::memcpy(static_cast<uint8_t*>(P.arenaMapped) + offset, data, size);
    cursor = offset + size;
    outOffset = offset;
    return true;
}

bool FrameArena::MakeIndexBuffer(const void* data, size_t bytes, VkBuffer& outBuf,
                                 VkDeviceSize& outOffset)
{
    if (Write(data, bytes, 4, outOffset))
    {
        outBuf = P.arena;
        return true;
    }
    ++overflows;
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (!R.MakeBuffer(bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, outBuf, m))
        return false;
    void* p = nullptr;
    VK_CHECK(vkMapMemory(R.device, m, 0, bytes, 0, &p));
    std::memcpy(p, data, bytes);
    vkUnmapMemory(R.device, m);
    keepBuffers.push_back(outBuf); keepMem.push_back(m);
    outOffset = 0;
    return true;
}

bool FrameArena::MakeUbo(const void* data, size_t size, VkDescriptorBufferInfo& out)
{
    const size_t bytes = std::max<size_t>(size, 16);
    VkDeviceSize offset = 0;
    if (Write(data, bytes, R.uniformOffsetAlignment, offset))
    {
        out = {P.arena, offset, bytes};
        return true;
    }
    ++overflows;
    VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE;
    if (!R.MakeBuffer(bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, b, m))
        return false;
    void* p = nullptr;
    VK_CHECK(vkMapMemory(R.device, m, 0, bytes, 0, &p));
    std::memcpy(p, data, size);
    vkUnmapMemory(R.device, m);
    keepBuffers.push_back(b); keepMem.push_back(m);
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
        lucent::info("draw", "per-draw arena overflowed on {} allocation(s):"
            " {} KiB fitted, {} KiB more was wanted, arena is {} KiB and will be"
            " sized to {} KiB", overflows, cursor / 1024,
            wanted / 1024, P.arenaBytes / 1024,
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
