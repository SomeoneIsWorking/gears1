#pragma once

#include <functional>

#include "swapchain.h"
#include "vulkan_device.h"

namespace gears::engine::render
{

// Shows rendered frames in the window, one frame in flight: each frame's
// commands are recorded by the caller, then its finished image is scaled
// onto the swapchain image (letterboxed to keep its aspect) and presented.
class WindowFrames
{
  public:
    WindowFrames(const VulkanDevice &device, VkExtent2D drawable);
    ~WindowFrames();
    WindowFrames(const WindowFrames &) = delete;
    WindowFrames &operator=(const WindowFrames &) = delete;

    // Waits for the previous frame, then records `render` (which leaves its
    // frame in `source`, TRANSFER_SRC_OPTIMAL, `source_extent` in size) and
    // presents the result. A frame the window has outgrown is dropped and
    // the swapchain rebuilt for `drawable`.
    void Draw(VkExtent2D drawable, const std::function<void(VkCommandBuffer)> &render,
              VkImage source, VkExtent2D source_extent);

  private:
    void RecordPresentation(VkImage source, VkExtent2D source_extent, VkImage target) const;

    const VulkanDevice &device_;
    Swapchain swapchain_;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    VkSemaphore image_ready_ = VK_NULL_HANDLE;
    VkSemaphore rendered_ = VK_NULL_HANDLE;
    VkFence finished_ = VK_NULL_HANDLE;
};

} // namespace gears::engine::render
