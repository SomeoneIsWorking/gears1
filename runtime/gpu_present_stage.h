#pragma once

#include <vulkan/vulkan.h>

namespace gears
{

// Owns the UNORM image used to move already-encoded display bytes into an sRGB
// swapchain without applying the transfer function a second time.
class SrgbRawCopyStage
{
  public:
    bool Create(VkDevice device, const VkPhysicalDeviceMemoryProperties &memory, VkExtent2D extent,
                VkFormat swapchainFormat);
    void Destroy(VkDevice device);
    VkImage Image() const { return image_; }

  private:
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
};

} // namespace gears
