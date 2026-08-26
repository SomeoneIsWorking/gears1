#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include <vulkan/vulkan.h>

#include "gpu_frame_capacity.h"

namespace gears::draw
{

// Mutable Vulkan resources that a submitted frame may still read. Persistent
// pipelines, images and texture caches remain renderer-owned; these resources
// are duplicated so the CPU may record frame N+1 while frame N executes.
struct GpuFrameResources
{
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    VkDescriptorPool drawDescriptors = VK_NULL_HANDLE;
    uint32_t drawDescriptorCapacity = 0;
    VkDescriptorPool resolveDescriptors = VK_NULL_HANDLE;
    uint32_t resolveDescriptorCapacity = 0;
    VkDescriptorPool reinterpretDescriptors = VK_NULL_HANDLE;
    uint32_t reinterpretDescriptorCapacity = 0;
    VkDescriptorPool depthAliasDescriptors = VK_NULL_HANDLE;

    VkBuffer arena = VK_NULL_HANDLE;
    VkDeviceMemory arenaMemory = VK_NULL_HANDLE;
    void *arenaMapped = nullptr;
    VkDeviceSize arenaBytes = 0;

    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    void *readbackMapped = nullptr;
    VkDeviceSize readbackBytes = 0;

    VkBuffer guestMemory = VK_NULL_HANDLE;
    VkDeviceMemory guestMemoryAllocation = VK_NULL_HANDLE;
    void *guestMemoryMapped = nullptr;
    VkDeviceSize guestMemoryBytes = 0;
};

// Vulkan adapter around GpuRetirement. A dedicated pump waits for any submitted
// fence so a title waiting on EVENT_WRITE_SHD can make progress even when no new
// frame arrives. User callbacks are delivered in submission order, regardless
// of which reusable slot index vkGetFenceStatus happens to visit first.
class GpuFrameSlots
{
  public:
    using Completion = std::function<void(bool)>;

    struct Lease
    {
        size_t slot = 0;
        uint64_t generation = 0;
        GpuFrameResources *resources = nullptr;
    };

    GpuFrameSlots();
    ~GpuFrameSlots();

    GpuFrameSlots(const GpuFrameSlots &) = delete;
    GpuFrameSlots &operator=(const GpuFrameSlots &) = delete;

    bool Initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily,
                    size_t capacity);

    // Waits only when every bounded slot is still in flight. With two slots and
    // CPU preparation longer than GPU execution, the steady path does not wait.
    std::optional<Lease> Acquire();
    void BeginTiming(const Lease &lease, VkCommandBuffer commands);
    void EndTiming(const Lease &lease, VkCommandBuffer commands);
    bool Submit(Lease lease, Completion completion);
    bool Cancel(Lease lease);

    // Explicit reset/teardown barrier. Normal frame progress is handled by the
    // completion pump and never calls the backend's blocking Drain operation.
    bool WaitInFlight();
    bool Drain();
    void Release();

    size_t InFlightCount() const;
    size_t Capacity() const;

  private:
    struct Impl;
    Impl *impl_ = nullptr;
};

} // namespace gears::draw
