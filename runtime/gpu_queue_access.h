#pragma once

#include <mutex>
#include <utility>

#include <vulkan/vulkan.h>

namespace gears
{

// Vulkan requires host access to one VkQueue to be externally synchronized.
// The renderer and presenter share one queue but run on different threads, so
// every queue operation and queue-wide idle barrier goes through this owner.
class GpuQueueAccess
{
  public:
    VkResult Submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence);
    VkResult Present(VkQueue queue, const VkPresentInfoKHR *present);
    VkResult WaitIdle(VkQueue queue);
    VkResult WaitDeviceIdle(VkDevice device);

    template <typename Operation>
    auto Invoke(Operation &&operation) -> decltype(std::forward<Operation>(operation)())
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return std::forward<Operation>(operation)();
    }

  private:
    std::mutex mutex_;
};

GpuQueueAccess &SharedGpuQueueAccess();

} // namespace gears
