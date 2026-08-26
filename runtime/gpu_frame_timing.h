#pragma once

#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan.h>

namespace gears::draw
{

struct GpuFrameTimingStats
{
    bool available = false;
    uint64_t samples = 0;
    uint64_t totalNanoseconds = 0;
    uint64_t maximumNanoseconds = 0;
    uint64_t failedSamples = 0;
};

// Vulkan timestamps retain only timestampValidBits from the selected queue
// family. Subtracting before masking preserves a duration across counter wrap.
constexpr uint64_t GpuTimestampElapsedTicks(uint64_t start, uint64_t end, uint32_t validBits)
{
    if (validBits == 0)
        return 0;
    const uint64_t mask = validBits >= 64 ? UINT64_MAX : (uint64_t{1} << validBits) - 1;
    return (end - start) & mask;
}

// One query pool per reusable renderer slot. Queries are read only after that
// slot's submission fence signals, so collecting a duration never adds a GPU
// wait to the producer or completion pump.
class GpuFrameTiming
{
  public:
    GpuFrameTiming();
    ~GpuFrameTiming();

    GpuFrameTiming(const GpuFrameTiming &) = delete;
    GpuFrameTiming &operator=(const GpuFrameTiming &) = delete;

    void Initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily,
                    size_t capacity);
    void Begin(VkCommandBuffer commands, size_t slot);
    void End(VkCommandBuffer commands, size_t slot);
    void Complete(size_t slot, bool success);
    void Release();

  private:
    struct Impl;
    Impl *impl_ = nullptr;
};

GpuFrameTimingStats CurrentGpuFrameTimingStats();

} // namespace gears::draw
