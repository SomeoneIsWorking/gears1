#include "gpu_queue_access.h"

namespace gears
{

VkResult GpuQueueAccess::Submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits,
                                VkFence fence)
{
    return Invoke([&] { return vkQueueSubmit(queue, count, submits, fence); });
}

VkResult GpuQueueAccess::Present(VkQueue queue, const VkPresentInfoKHR *present)
{
    return Invoke([&] { return vkQueuePresentKHR(queue, present); });
}

VkResult GpuQueueAccess::WaitIdle(VkQueue queue)
{
    return Invoke([&] { return vkQueueWaitIdle(queue); });
}

VkResult GpuQueueAccess::WaitDeviceIdle(VkDevice device)
{
    return Invoke([&] { return vkDeviceWaitIdle(device); });
}

GpuQueueAccess &SharedGpuQueueAccess()
{
    static GpuQueueAccess access;
    return access;
}

} // namespace gears
