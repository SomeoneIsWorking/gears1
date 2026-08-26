#include "gpu_frame_timing.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>

#include <lucent/log.h>

namespace gears::draw
{
namespace
{

std::atomic<uint64_t> g_samples{0};
std::atomic<uint64_t> g_totalNanoseconds{0};
std::atomic<uint64_t> g_maximumNanoseconds{0};
std::atomic<uint64_t> g_failedSamples{0};
std::atomic<bool> g_available{false};

void UpdateMaximum(uint64_t nanoseconds)
{
    uint64_t previous = g_maximumNanoseconds.load(std::memory_order_relaxed);
    while (previous < nanoseconds && !g_maximumNanoseconds.compare_exchange_weak(
                                         previous, nanoseconds, std::memory_order_relaxed))
    {
    }
}

} // namespace

struct GpuFrameTiming::Impl
{
    VkDevice device = VK_NULL_HANDLE;
    std::vector<VkQueryPool> pools;
    float timestampPeriod = 0.0f;
    uint32_t timestampValidBits = 0;
};

GpuFrameTiming::GpuFrameTiming() = default;

GpuFrameTiming::~GpuFrameTiming()
{
    if (impl_)
        std::abort();
}

void GpuFrameTiming::Initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily,
                                size_t capacity)
{
    g_available.store(false, std::memory_order_relaxed);
    if (impl_ || physical == VK_NULL_HANDLE || device == VK_NULL_HANDLE || capacity == 0)
        return;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    if (queueFamily >= familyCount)
    {
        lucent::warn("draw", "GPU frame timing disabled: queue family {} is out of range ({})",
                     queueFamily, familyCount);
        return;
    }
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    const uint32_t validBits = families[queueFamily].timestampValidBits;
    if (validBits == 0 || properties.limits.timestampPeriod <= 0.0f)
    {
        lucent::warn("draw",
                     "GPU frame timing unavailable: queue family {} exposes {} timestamp bits"
                     " with a {} ns period",
                     queueFamily, validBits, properties.limits.timestampPeriod);
        return;
    }

    Impl *timing = new Impl;
    timing->device = device;
    timing->timestampPeriod = properties.limits.timestampPeriod;
    timing->timestampValidBits = validBits;
    timing->pools.resize(capacity, VK_NULL_HANDLE);
    VkQueryPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    createInfo.queryCount = 2;
    for (VkQueryPool &pool : timing->pools)
    {
        if (vkCreateQueryPool(device, &createInfo, nullptr, &pool) == VK_SUCCESS)
            continue;
        for (VkQueryPool created : timing->pools)
            vkDestroyQueryPool(device, created, nullptr);
        delete timing;
        lucent::warn("draw", "GPU frame timing disabled: timestamp query-pool creation failed");
        return;
    }

    g_samples.store(0, std::memory_order_relaxed);
    g_totalNanoseconds.store(0, std::memory_order_relaxed);
    g_maximumNanoseconds.store(0, std::memory_order_relaxed);
    g_failedSamples.store(0, std::memory_order_relaxed);
    g_available.store(true, std::memory_order_relaxed);
    impl_ = timing;
    lucent::info("draw", "GPU frame timing enabled: {} slots, {} valid bits, {} ns/tick", capacity,
                 validBits, properties.limits.timestampPeriod);
}

void GpuFrameTiming::Begin(VkCommandBuffer commands, size_t slot)
{
    if (!impl_ || slot >= impl_->pools.size())
        return;
    const VkQueryPool pool = impl_->pools[slot];
    vkCmdResetQueryPool(commands, pool, 0, 2);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
}

void GpuFrameTiming::End(VkCommandBuffer commands, size_t slot)
{
    if (!impl_ || slot >= impl_->pools.size())
        return;
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, impl_->pools[slot], 1);
}

void GpuFrameTiming::Complete(size_t slot, bool success)
{
    if (!impl_ || slot >= impl_->pools.size() || !success)
        return;
    uint64_t timestamps[2]{};
    const VkResult result =
        vkGetQueryPoolResults(impl_->device, impl_->pools[slot], 0, 2, sizeof(timestamps),
                              timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS)
    {
        g_failedSamples.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint64_t ticks =
        GpuTimestampElapsedTicks(timestamps[0], timestamps[1], impl_->timestampValidBits);
    const long double duration = static_cast<long double>(ticks) * impl_->timestampPeriod;
    const uint64_t nanoseconds = static_cast<uint64_t>(
        std::min(duration, static_cast<long double>(std::numeric_limits<uint64_t>::max())));
    g_totalNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
    UpdateMaximum(nanoseconds);
    g_samples.fetch_add(1, std::memory_order_relaxed);
}

void GpuFrameTiming::Release()
{
    if (!impl_)
        return;
    for (VkQueryPool pool : impl_->pools)
        vkDestroyQueryPool(impl_->device, pool, nullptr);
    delete impl_;
    impl_ = nullptr;
}

GpuFrameTimingStats CurrentGpuFrameTimingStats()
{
    return {.available = g_available.load(std::memory_order_relaxed),
            .samples = g_samples.load(std::memory_order_relaxed),
            .totalNanoseconds = g_totalNanoseconds.load(std::memory_order_relaxed),
            .maximumNanoseconds = g_maximumNanoseconds.load(std::memory_order_relaxed),
            .failedSamples = g_failedSamples.load(std::memory_order_relaxed)};
}

} // namespace gears::draw
