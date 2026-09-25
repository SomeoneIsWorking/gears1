#pragma once

#include <cstdint>
#include <vector>

#include "device_image.h"
#include "vulkan_device.h"

namespace gears::engine::render
{

// The color and depth attachments of a headless frame, the render pass that
// clears and draws into them, and the copy that reads the color back.
class OffscreenTarget
{
  public:
    static constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_SRGB;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    OffscreenTarget(const VulkanDevice &device, VkExtent2D extent);
    ~OffscreenTarget();
    OffscreenTarget(const OffscreenTarget &) = delete;
    OffscreenTarget &operator=(const OffscreenTarget &) = delete;

    [[nodiscard]] VkExtent2D Extent() const noexcept { return extent_; }
    [[nodiscard]] VkRenderPass RenderPass() const noexcept { return render_pass_; }

    void Begin(VkCommandBuffer commands) const;
    // Ends the pass and copies the color attachment into the readback buffer.
    void EndAndCopy(VkCommandBuffer commands) const;
    // RGBA8 rows of the last copied frame; valid after the submit completes.
    [[nodiscard]] std::vector<std::uint8_t> Pixels();

  private:
    const VulkanDevice &device_;
    VkExtent2D extent_;
    DeviceImage color_;
    DeviceImage depth_;
    HostBuffer readback_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
};

} // namespace gears::engine::render
