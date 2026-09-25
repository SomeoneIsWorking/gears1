#include "device_image.h"

namespace gears::engine::render
{

DeviceImage::DeviceImage(const VulkanDevice &device, VkFormat format, VkExtent2D extent,
                         VkImageUsageFlags usage, VkImageAspectFlags aspect,
                         std::uint32_t mip_levels, VkComponentMapping components)
    : device_(device.Device())
{
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = mip_levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(device_, &info, nullptr, &image_), "vkCreateImage");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image_, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        device.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device_, &allocate, nullptr, &memory_), "vkAllocateMemory");
    Check(vkBindImageMemory(device_, image_, memory_, 0), "vkBindImageMemory");
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.components = components;
    view.subresourceRange = {aspect, 0, mip_levels, 0, 1};
    Check(vkCreateImageView(device_, &view, nullptr, &view_), "vkCreateImageView");
}

DeviceImage::~DeviceImage()
{
    vkDestroyImageView(device_, view_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
}

} // namespace gears::engine::render
