#include "gpu_present_stage.h"

#include <cstdint>

#include <lucent/log.h>

#include "swapchain_format.h"

namespace gears
{

bool SrgbRawCopyStage::Create(VkDevice device, const VkPhysicalDeviceMemoryProperties &memory,
                              VkExtent2D extent, VkFormat swapchainFormat)
{
    const VkFormat stageFormat = SwapchainSrgbStageFormat(swapchainFormat);
    if (stageFormat == VK_FORMAT_UNDEFINED)
    {
        lucent::warn("present",
                     "sRGB swapchain format {} has no known"
                     " component-identical UNORM stage; direct blit will encode twice",
                     uint32_t(swapchainFormat));
        return false;
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = stageFormat;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(device, &imageInfo, nullptr, &image_) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image_, &requirements);
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
    {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            memoryType = i;
            break;
        }
    }
    if (memoryType == UINT32_MAX)
    {
        Destroy(device);
        return false;
    }

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device, &allocation, nullptr, &memory_) != VK_SUCCESS ||
        vkBindImageMemory(device, image_, memory_, 0) != VK_SUCCESS)
    {
        Destroy(device);
        return false;
    }

    lucent::info("present",
                 "swapchain is sRGB-tagged: raw bytes pass through"
                 " matching UNORM format {} without encoding or channel reorder",
                 uint32_t(stageFormat));
    return true;
}

void SrgbRawCopyStage::Destroy(VkDevice device)
{
    vkDestroyImage(device, image_, nullptr);
    vkFreeMemory(device, memory_, nullptr);
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
}

} // namespace gears
