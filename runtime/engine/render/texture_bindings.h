#pragma once

#include <cstdint>

#include "gpu_texture.h"
#include "vulkan_device.h"

namespace gears::engine::render
{

// The descriptor layout the mesh pipeline samples its base colour through,
// one trilinear repeating sampler, and a pool of one set per bound texture.
class TextureBindings
{
  public:
    TextureBindings(const VulkanDevice &device, std::uint32_t capacity);
    ~TextureBindings();
    TextureBindings(const TextureBindings &) = delete;
    TextureBindings &operator=(const TextureBindings &) = delete;

    [[nodiscard]] VkDescriptorSetLayout Layout() const noexcept { return layout_; }
    // A set sampling `texture`; refuses once `capacity` sets are allocated.
    [[nodiscard]] VkDescriptorSet Bind(const GpuTexture &texture);

  private:
    VkDevice device_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
};

} // namespace gears::engine::render
