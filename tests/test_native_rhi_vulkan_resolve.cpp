#include "native_rhi_vulkan_resolve.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{

struct VulkanTestContext
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
};

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

bool Check(VkResult result, const char *operation)
{
    if (result == VK_SUCCESS)
        return true;
    std::fprintf(stderr, "native Vulkan resolve: %s failed (%d)\n", operation, result);
    return false;
}

bool FindMemoryType(VkPhysicalDevice physical, std::uint32_t bits, VkMemoryPropertyFlags properties,
                    std::uint32_t &index)
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    for (std::uint32_t candidate = 0; candidate < memoryProperties.memoryTypeCount; ++candidate)
    {
        if ((bits & (1u << candidate)) != 0 &&
            (memoryProperties.memoryTypes[candidate].propertyFlags & properties) == properties)
        {
            index = candidate;
            return true;
        }
    }
    return false;
}

void Transition(VkCommandBuffer commands, VkImage image, VkImageLayout oldLayout,
                VkImageLayout newLayout, VkAccessFlags sourceAccess,
                VkAccessFlags destinationAccess)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool MakeImage(VulkanTestContext &context, VkPhysicalDevice physical, VkExtent2D extent, Image &out)
{
    VkImageCreateInfo createInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    createInfo.imageType = VK_IMAGE_TYPE_2D;
    createInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    createInfo.extent = {extent.width, extent.height, 1};
    createInfo.mipLevels = 1;
    createInfo.arrayLayers = 1;
    createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    createInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!Check(vkCreateImage(context.device, &createInfo, nullptr, &out.image), "vkCreateImage"))
        return false;

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context.device, out.image, &requirements);
    std::uint32_t memoryType = 0;
    if (!FindMemoryType(physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        memoryType))
        return false;
    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    return Check(vkAllocateMemory(context.device, &allocateInfo, nullptr, &out.memory),
                 "vkAllocateMemory") &&
           Check(vkBindImageMemory(context.device, out.image, out.memory, 0), "vkBindImageMemory");
}

bool MakeBuffer(VulkanTestContext &context, VkPhysicalDevice physical, VkDeviceSize size,
                Buffer &out)
{
    VkBufferCreateInfo createInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    createInfo.size = size;
    createInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(vkCreateBuffer(context.device, &createInfo, nullptr, &out.buffer), "vkCreateBuffer"))
        return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context.device, out.buffer, &requirements);
    std::uint32_t memoryType = 0;
    if (!FindMemoryType(physical, requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        memoryType))
        return false;
    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    return Check(vkAllocateMemory(context.device, &allocateInfo, nullptr, &out.memory),
                 "vkAllocateMemory") &&
           Check(vkBindBufferMemory(context.device, out.buffer, out.memory, 0),
                 "vkBindBufferMemory");
}

void Destroy(VulkanTestContext &context, Image &image)
{
    if (image.image != VK_NULL_HANDLE)
        vkDestroyImage(context.device, image.image, nullptr);
    if (image.memory != VK_NULL_HANDLE)
        vkFreeMemory(context.device, image.memory, nullptr);
    image = {};
}

void Destroy(VulkanTestContext &context, Buffer &buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(context.device, buffer.buffer, nullptr);
    if (buffer.memory != VK_NULL_HANDLE)
        vkFreeMemory(context.device, buffer.memory, nullptr);
    buffer = {};
}

} // namespace

int main()
{
    VulkanTestContext context;
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "gears-native-rhi-resolve-test";
    appInfo.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    if (!Check(vkCreateInstance(&instanceInfo, nullptr, &context.instance), "vkCreateInstance"))
        return 1;

    std::uint32_t physicalCount = 0;
    if (!Check(vkEnumeratePhysicalDevices(context.instance, &physicalCount, nullptr),
               "vkEnumeratePhysicalDevices count") ||
        physicalCount == 0)
        return 1;
    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
    if (!Check(vkEnumeratePhysicalDevices(context.instance, &physicalCount, physicalDevices.data()),
               "vkEnumeratePhysicalDevices"))
        return 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::uint32_t familyCount = 0;
    for (VkPhysicalDevice candidate : physicalDevices)
    {
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        for (std::uint32_t family = 0; family < familyCount; ++family)
        {
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                families[family].queueCount != 0)
            {
                physical = candidate;
                context.queueFamily = family;
                break;
            }
        }
        if (physical != VK_NULL_HANDLE)
            break;
    }
    if (physical == VK_NULL_HANDLE)
        return 1;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = context.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    if (!Check(vkCreateDevice(physical, &deviceInfo, nullptr, &context.device), "vkCreateDevice"))
        return 1;
    vkGetDeviceQueue(context.device, context.queueFamily, 0, &context.queue);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = context.queueFamily;
    if (!Check(vkCreateCommandPool(context.device, &poolInfo, nullptr, &context.commandPool),
               "vkCreateCommandPool"))
        return 1;

    constexpr VkExtent2D extent{8, 8};
    Image source;
    Image destination;
    Buffer readback;
    if (!MakeImage(context, physical, extent, source) ||
        !MakeImage(context, physical, extent, destination) ||
        !MakeBuffer(context, physical, extent.width * extent.height * 4, readback))
        return 1;

    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = context.commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (!Check(vkAllocateCommandBuffers(context.device, &commandInfo, &commands),
               "vkAllocateCommandBuffers"))
        return 1;
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!Check(vkBeginCommandBuffer(commands, &beginInfo), "vkBeginCommandBuffer"))
        return 1;

    Transition(commands, source.image, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue clear{{0x12 / 255.0f, 0x34 / 255.0f, 0x56 / 255.0f, 0x78 / 255.0f}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(commands, source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                         &range);
    Transition(commands, source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_TRANSFER_READ_BIT);

    gears::RhiSemanticResolve resolve;
    resolve.sourceObject = 1;
    resolve.destinationObject = 2;
    resolve.sourceRectangle = {1, 2, 4, 3};
    resolve.destinationPoint = {2, 3};
    resolve.destinationPitch = extent.width;
    resolve.destinationHeight = extent.height - resolve.destinationPoint[1];
    resolve.destinationFormat = 0;
    resolve.bytesPerBlock = 4;
    gears::native_rhi::VulkanResolveImage nativeSource{source.image, VK_FORMAT_R8G8B8A8_UNORM,
                                                       extent, VK_SAMPLE_COUNT_1_BIT,
                                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    gears::native_rhi::VulkanResolveImage nativeDestination{
        destination.image, VK_FORMAT_R8G8B8A8_UNORM, extent, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED};
    assert(
        gears::native_rhi::RecordColorResolve(commands, resolve, nativeSource, nativeDestination) ==
        gears::native_rhi::VulkanResolveStatus::Accepted);
    gears::RhiSemanticResolve unsupported = resolve;
    unsupported.operationFlags = 8;
    assert(gears::native_rhi::RecordColorResolve(commands, unsupported, nativeSource,
                                                 nativeDestination) ==
           gears::native_rhi::VulkanResolveStatus::UnsupportedOperationFlags);

    Transition(commands, destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy readbackRegion{};
    readbackRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    readbackRegion.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(commands, destination.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &readbackRegion);
    if (!Check(vkEndCommandBuffer(commands), "vkEndCommandBuffer"))
        return 1;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commands;
    if (!Check(vkQueueSubmit(context.queue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit") ||
        !Check(vkQueueWaitIdle(context.queue), "vkQueueWaitIdle"))
        return 1;

    void *mapped = nullptr;
    if (!Check(vkMapMemory(context.device, readback.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
               "vkMapMemory"))
        return 1;
    const auto *pixels = static_cast<const std::uint8_t *>(mapped);
    for (std::uint32_t y = 0; y < 3; ++y)
        for (std::uint32_t x = 0; x < 4; ++x)
        {
            const std::size_t offset = ((y + 3) * extent.width + x + 2) * 4;
            assert(pixels[offset + 0] == 0x12);
            assert(pixels[offset + 1] == 0x34);
            assert(pixels[offset + 2] == 0x56);
            assert(pixels[offset + 3] == 0x78);
        }
    vkUnmapMemory(context.device, readback.memory);

    vkDeviceWaitIdle(context.device);
    Destroy(context, readback);
    Destroy(context, destination);
    Destroy(context, source);
    vkDestroyCommandPool(context.device, context.commandPool, nullptr);
    vkDestroyDevice(context.device, nullptr);
    vkDestroyInstance(context.instance, nullptr);
    return 0;
}
