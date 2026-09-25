#pragma once

#include "scene/transform.h"
#include "vulkan_device.h"

namespace gears::engine::render
{

// The values every draw of a frame shares, in one host-visible uniform
// buffer bound as descriptor set 1: the view-projection matrix.
class FrameConstants
{
  public:
    explicit FrameConstants(const VulkanDevice &device);
    ~FrameConstants();
    FrameConstants(const FrameConstants &) = delete;
    FrameConstants &operator=(const FrameConstants &) = delete;

    [[nodiscard]] VkDescriptorSetLayout Layout() const noexcept { return layout_; }
    [[nodiscard]] VkDescriptorSet Set() const noexcept { return set_; }

    // Stores the frame's values; call before submitting the frame's commands.
    void Write(const scene::Matrix &view_projection);

  private:
    VkDevice device_;
    HostBuffer buffer_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};

} // namespace gears::engine::render
