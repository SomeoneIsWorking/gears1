#include "native_rhi_vulkan_resources.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <utility>
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

bool Check(VkResult result, const char *operation)
{
    if (result == VK_SUCCESS)
        return true;
    std::fprintf(stderr, "native Vulkan resolve: %s failed (%d)\n", operation, result);
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
    gears::native_rhi::VulkanResourceOwner resources(physical, context.device);
    const auto invalidBuffer = resources.CreateBuffer({});
    const auto invalidImage = resources.CreateImage({});
    assert(invalidBuffer.status == gears::native_rhi::VulkanResourceStatus::InvalidDescriptor);
    assert(invalidImage.status == gears::native_rhi::VulkanResourceStatus::InvalidDescriptor);
    assert(resources.RetireBuffer(0) == nullptr);
    assert(resources.RetireImage(0) == nullptr);
    const gears::native_rhi::VulkanImageDescriptor imageDescriptor{
        VK_FORMAT_R8G8B8A8_UNORM,
        extent,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    auto sourceResult = resources.CreateImage(imageDescriptor);
    auto destinationResult = resources.CreateImage(imageDescriptor);
    const gears::native_rhi::VulkanBufferDescriptor readbackDescriptor{
        extent.width * extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true};
    auto readbackResult = resources.CreateBuffer(readbackDescriptor);
    if (sourceResult.status != gears::native_rhi::VulkanResourceStatus::Accepted ||
        destinationResult.status != gears::native_rhi::VulkanResourceStatus::Accepted ||
        readbackResult.status != gears::native_rhi::VulkanResourceStatus::Accepted)
        return 1;
    auto source = std::move(sourceResult.resource);
    auto destination = std::move(destinationResult.resource);
    auto readback = std::move(readbackResult.resource);

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

    Transition(commands, source->resolve.image, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue clear{{0x12 / 255.0f, 0x34 / 255.0f, 0x56 / 255.0f, 0x78 / 255.0f}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(commands, source->resolve.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear, 1, &range);
    Transition(commands, source->resolve.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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
    assert(gears::native_rhi::RecordColorResolve(commands, resolve, source->resolve,
                                                 destination->resolve) ==
           gears::native_rhi::VulkanResolveStatus::Accepted);
    gears::RhiSemanticResolve unsupported = resolve;
    unsupported.operationFlags = 8;
    assert(gears::native_rhi::RecordColorResolve(commands, unsupported, source->resolve,
                                                 destination->resolve) ==
           gears::native_rhi::VulkanResolveStatus::UnsupportedOperationFlags);

    Transition(commands, destination->resolve.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy readbackRegion{};
    readbackRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    readbackRegion.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(commands, destination->resolve.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1,
                           &readbackRegion);
    if (!Check(vkEndCommandBuffer(commands), "vkEndCommandBuffer"))
        return 1;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commands;
    if (!Check(vkQueueSubmit(context.queue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit") ||
        !Check(vkQueueWaitIdle(context.queue), "vkQueueWaitIdle"))
        return 1;

    const auto *pixels = static_cast<const std::uint8_t *>(readback->mapped);
    assert(pixels != nullptr);
    for (std::uint32_t y = 0; y < 3; ++y)
        for (std::uint32_t x = 0; x < 4; ++x)
        {
            const std::size_t offset = ((y + 3) * extent.width + x + 2) * 4;
            assert(pixels[offset + 0] == 0x12);
            assert(pixels[offset + 1] == 0x34);
            assert(pixels[offset + 2] == 0x56);
            assert(pixels[offset + 3] == 0x78);
        }
    vkDeviceWaitIdle(context.device);
    auto retiredSource = resources.RetireImage(source->id);
    assert(retiredSource != nullptr);
    assert(resources.AcquireImage(source->id) == nullptr);
    assert(resources.ActiveImageCount() == 1);
    assert(resources.ActiveBufferCount() == 1);
    auto retiredDestination = resources.RetireImage(destination->id);
    auto retiredReadback = resources.RetireBuffer(readback->id);
    assert(retiredDestination != nullptr);
    assert(retiredReadback != nullptr);
    source.reset();
    destination.reset();
    readback.reset();
    retiredSource.reset();
    retiredDestination.reset();
    retiredReadback.reset();
    vkDestroyCommandPool(context.device, context.commandPool, nullptr);
    vkDestroyDevice(context.device, nullptr);
    vkDestroyInstance(context.instance, nullptr);
    return 0;
}
