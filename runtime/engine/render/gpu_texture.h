#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "device_image.h"
#include "package/content_files.h"
#include "texture/texture2d.h"
#include "vulkan_device.h"

namespace gears::engine::render
{

// A sampled texture in device memory: every stored mip of a Texture2D in its
// own block format (colour formats as sRGB), or one solid texel.
class GpuTexture
{
  public:
    // Whether a stored format has a sampled equivalent.
    [[nodiscard]] static bool CanSample(texture::PixelFormat format) noexcept;

    // Uploads the texture's mip chain from its largest stored mip down to
    // the last one that halves the one before. Refuses a format with no
    // sampled equivalent.
    GpuTexture(const VulkanDevice &device, const texture::Texture2D &texture,
               package::ContentFiles &files);
    // One texel of an sRGB colour, for sections whose material has no texture.
    GpuTexture(const VulkanDevice &device, std::array<std::uint8_t, 4> rgba);

    [[nodiscard]] VkImageView View() const noexcept { return image_->View(); }

  private:
    std::unique_ptr<DeviceImage> image_;
};

} // namespace gears::engine::render
