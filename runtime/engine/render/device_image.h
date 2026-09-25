#pragma once

#include <cstdint>

#include "vulkan_device.h"

namespace gears::engine::render
{

// A device-local 2D image with its memory and one view of all its mips.
class DeviceImage
{
  public:
    DeviceImage(const VulkanDevice &device, VkFormat format, VkExtent2D extent,
                VkImageUsageFlags usage, VkImageAspectFlags aspect, std::uint32_t mip_levels = 1,
                VkComponentMapping components = {});
    ~DeviceImage();
    DeviceImage(const DeviceImage &) = delete;
    DeviceImage &operator=(const DeviceImage &) = delete;

    [[nodiscard]] VkImage Image() const noexcept { return image_; }
    [[nodiscard]] VkImageView View() const noexcept { return view_; }

  private:
    VkDevice device_;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
};

} // namespace gears::engine::render
