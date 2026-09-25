#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "vulkan_device.h"

namespace gears::engine::render
{

// The window's presentable images: an sRGB swapchain on the device's
// surface, sized to the window's drawable area and recreated when the
// window changes.
class Swapchain
{
  public:
    // Refuses a device with no surface.
    Swapchain(const VulkanDevice &device, VkExtent2D drawable);
    ~Swapchain();
    Swapchain(const Swapchain &) = delete;
    Swapchain &operator=(const Swapchain &) = delete;

    // Replaces the images for a new drawable size; waits for the device.
    void Recreate(VkExtent2D drawable);

    // The next image to draw, signalling `ready` when it may be written;
    // none when the swapchain no longer matches the window.
    [[nodiscard]] std::optional<std::uint32_t> Acquire(VkSemaphore ready);
    // Queues `image` for display once `rendered` signals; false when the
    // swapchain no longer matches the window.
    [[nodiscard]] bool Present(std::uint32_t image, VkSemaphore rendered);

    [[nodiscard]] VkExtent2D Extent() const noexcept { return extent_; }
    [[nodiscard]] VkImage Image(std::uint32_t index) const { return images_.at(index); }

  private:
    void Create(VkExtent2D drawable, VkSwapchainKHR previous);

    const VulkanDevice &device_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
};

} // namespace gears::engine::render
