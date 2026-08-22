#include "gpu_scanout.h"

#include <lucent/log.h>

#include "gpu_draw_renderer.h"
#include "gpu_shared_device.h"

namespace gears::draw
{

bool GpuScanout::Initialize(Renderer &renderer, uint32_t width, uint32_t height)
{
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!renderer.ownsDevice)
        imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

    for (uint32_t i = 0; i < 2; ++i)
    {
        if (vkCreateImage(renderer.device, &imageInfo, nullptr, &images_[i]) != VK_SUCCESS)
            return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(renderer.device, images_[i], &requirements);
        uint32_t memoryType = 0;
        if (!renderer.FindMemory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 memoryType) &&
            !renderer.FindMemory(requirements.memoryTypeBits, 0, memoryType))
            return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(renderer.device, &allocation, nullptr, &memory_[i]) != VK_SUCCESS ||
            vkBindImageMemory(renderer.device, images_[i], memory_[i], 0) != VK_SUCCESS)
            return false;
    }

    if (!renderer.ownsDevice && !gamma_.Initialize(renderer, images_))
        return false;
    return true;
}

bool GpuScanout::Record(Renderer &renderer, VkCommandBuffer commands, VkImage source,
                        uint32_t width, uint32_t height, const uint32_t *guestGammaRamp,
                        VkBuffer readback, bool copyToHost, GpuScanoutResult &result)
{
    const uint32_t imageIndex = nextImage_;
    nextImage_ ^= 1u;
    VkImage destination = images_[imageIndex];
    if (destination == VK_NULL_HANDLE)
        return false;

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = destination;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    // Source surface allocations include MSAA storage rows, but resolved image
    // content occupies the top display-sized rectangle (catalog #86).
    blit.srcOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
    blit.dstOffsets[1] = blit.srcOffsets[1];
    vkCmdBlitImage(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

    result.gammaApplied = false;
    if (guestGammaRamp && !renderer.ownsDevice)
    {
        const ScanoutGammaLut lut = BuildScanoutGammaLut(guestGammaRamp);
        if (!gamma_.Apply(renderer, commands, imageIndex, width, height, lut))
        {
            lucent::error("draw", "shared-device scan-out gamma refused the"
                                  " frame; publishing it without the guest LUT would be wrong");
            return false;
        }
        result.gammaApplied = true;
    }
    else
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    }

    if (copyToHost)
    {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(commands, destination, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback, 1, &region);
    }
    result.image = destination;
    return true;
}

bool GpuScanout::Publish(const GpuScanoutResult &result, uint32_t width, uint32_t height,
                         long frameId)
{
    if (result.image == VK_NULL_HANDLE || frameId <= 0)
    {
        lucent::error("draw", "refusing shared scan-out without a valid guest frame identity");
        return false;
    }
    SharedFrameImage frame;
    frame.image = result.image;
    frame.width = width;
    frame.height = height;
    frame.sequence = static_cast<uint64_t>(frameId);
    return PublishSharedFrameImage(frame);
}

void GpuScanout::Release(Renderer &renderer)
{
    gamma_.Release(renderer.device);
    for (uint32_t i = 0; i < 2; ++i)
    {
        vkDestroyImage(renderer.device, images_[i], nullptr);
        vkFreeMemory(renderer.device, memory_[i], nullptr);
        images_[i] = VK_NULL_HANDLE;
        memory_[i] = VK_NULL_HANDLE;
    }
}

} // namespace gears::draw
