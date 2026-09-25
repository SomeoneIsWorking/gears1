#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>

#include "gpu_texture.h"
#include "vulkan_device.h"

namespace gears::engine::render
{

// Light-map coefficient textures a draw samples.
inline constexpr std::size_t kLightMapTextures = 3;

// The textures one draw samples.
struct DrawTextures
{
    const GpuTexture *color = nullptr;
    const GpuTexture *opacity = nullptr;
    std::array<const GpuTexture *, kLightMapTextures> light_map{};

    [[nodiscard]] bool operator<(const DrawTextures &other) const noexcept
    {
        return std::tie(color, opacity, light_map) <
               std::tie(other.color, other.opacity, other.light_map);
    }
};

// The descriptor layout the mesh pipeline samples a material through (base
// colour at binding 0, opacity at binding 1, the light-map coefficients as
// an array at binding 2), one trilinear repeating sampler, and a pool of one
// set per bound combination.
class TextureBindings
{
  public:
    TextureBindings(const VulkanDevice &device, std::uint32_t capacity);
    ~TextureBindings();
    TextureBindings(const TextureBindings &) = delete;
    TextureBindings &operator=(const TextureBindings &) = delete;

    [[nodiscard]] VkDescriptorSetLayout Layout() const noexcept { return layout_; }
    // A set sampling `textures`, every one of which is set; refuses once
    // `capacity` sets are allocated.
    [[nodiscard]] VkDescriptorSet Bind(const DrawTextures &textures);

  private:
    static constexpr std::uint32_t kDescriptorsPerSet = 2U + kLightMapTextures;

    VkDevice device_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
};

} // namespace gears::engine::render
