#include "gpu_texture.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

#include "package/byte_reader.h"

namespace gears::engine::render
{
namespace
{

// The sampled format a stored format uploads to, unchanged in its bytes;
// VK_FORMAT_UNDEFINED for none.
VkFormat SampledFormat(texture::PixelFormat format) noexcept
{
    switch (format)
    {
    case texture::PixelFormat::Dxt1:
        return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case texture::PixelFormat::Dxt3:
        return VK_FORMAT_BC2_SRGB_BLOCK;
    case texture::PixelFormat::Dxt5:
        return VK_FORMAT_BC3_SRGB_BLOCK;
    case texture::PixelFormat::A8R8G8B8:
        // B, G, R, A bytes once each word is in host order.
        return VK_FORMAT_B8G8R8A8_SRGB;
    case texture::PixelFormat::G8:
        return VK_FORMAT_R8_UNORM;
    default:
        break;
    }
    return VK_FORMAT_UNDEFINED;
}

// Luminance reads as grey.
VkComponentMapping ComponentsOf(texture::PixelFormat format) noexcept
{
    if (format == texture::PixelFormat::G8)
    {
        return {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                VK_COMPONENT_SWIZZLE_ONE};
    }
    return {};
}

struct MipUpload
{
    VkExtent2D extent;
    std::vector<std::uint8_t> bytes;
};

// Copies staged mips into a fresh image and leaves it ready for sampling.
void Upload(const VulkanDevice &device, const DeviceImage &image,
            const std::vector<MipUpload> &mips)
{
    std::size_t total = 0;
    for (const MipUpload &mip : mips)
    {
        total += mip.bytes.size();
    }
    HostBuffer staging = device.CreateHostBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::vector<VkBufferImageCopy> copies;
    std::size_t offset = 0;
    for (std::size_t level = 0; level < mips.size(); ++level)
    {
        std::memcpy(staging.Bytes().data() + offset, mips[level].bytes.data(),
                    mips[level].bytes.size());
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<std::uint32_t>(level), 0,
                                 1};
        copy.imageExtent = {mips[level].extent.width, mips[level].extent.height, 1};
        copies.push_back(copy);
        offset += mips[level].bytes.size();
    }
    auto levels = static_cast<std::uint32_t>(mips.size());
    device.Submit(
        [&](VkCommandBuffer commands)
        {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.Image();
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &barrier);
            vkCmdCopyBufferToImage(commands, staging.Handle(), image.Image(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(copies.size()), copies.data());
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &barrier);
        });
}

std::vector<MipUpload> MipChain(const texture::Texture2D &texture, package::ContentFiles &files)
{
    const auto &stored = texture.Mips();
    if (stored.empty())
    {
        throw package::PackageFormatError("texture stores no mips");
    }
    std::vector<MipUpload> chain;
    for (std::size_t i = 0; i < stored.size(); ++i)
    {
        VkExtent2D extent{static_cast<std::uint32_t>(stored[i].width),
                          static_cast<std::uint32_t>(stored[i].height)};
        if (i > 0U && (extent.width != std::max(chain.back().extent.width / 2U, 1U) ||
                       extent.height != std::max(chain.back().extent.height / 2U, 1U)))
        {
            break;
        }
        chain.push_back({extent, texture.LinearMip(i, files)});
    }
    return chain;
}

} // namespace

bool GpuTexture::CanSample(texture::PixelFormat format) noexcept
{
    return SampledFormat(format) != VK_FORMAT_UNDEFINED;
}

GpuTexture::GpuTexture(const VulkanDevice &device, const texture::Texture2D &texture,
                       package::ContentFiles &files)
{
    VkFormat format = SampledFormat(texture.Format());
    if (format == VK_FORMAT_UNDEFINED)
    {
        throw package::PackageFormatError(std::format("texture format {} has no sampled equivalent",
                                                      texture::NameOf(texture.Format())));
    }
    std::vector<MipUpload> chain = MipChain(texture, files);
    image_ = std::make_unique<DeviceImage>(
        device, format, chain.front().extent,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
        static_cast<std::uint32_t>(chain.size()), ComponentsOf(texture.Format()));
    Upload(device, *image_, chain);
}

GpuTexture::GpuTexture(const VulkanDevice &device, std::array<std::uint8_t, 4> rgba)
    : image_(std::make_unique<DeviceImage>(
          device, VK_FORMAT_R8G8B8A8_SRGB, VkExtent2D{1, 1},
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
{
    Upload(device, *image_, {{VkExtent2D{1, 1}, {rgba.begin(), rgba.end()}}});
}

} // namespace gears::engine::render
