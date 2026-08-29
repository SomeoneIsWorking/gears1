#include "native_rhi_vulkan_resources.h"

#include <utility>

namespace gears::native_rhi
{
namespace
{

void DestroyBuffer(VulkanBufferResource &resource)
{
    if (resource.mapped != nullptr)
        vkUnmapMemory(resource.device, resource.memory);
    if (resource.buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(resource.device, resource.buffer, nullptr);
    if (resource.memory != VK_NULL_HANDLE)
        vkFreeMemory(resource.device, resource.memory, nullptr);
}

void DestroyImage(VulkanImageResource &resource)
{
    if (resource.resolve.image != VK_NULL_HANDLE)
        vkDestroyImage(resource.device, resource.resolve.image, nullptr);
    if (resource.memory != VK_NULL_HANDLE)
        vkFreeMemory(resource.device, resource.memory, nullptr);
}

} // namespace

VulkanBufferResource::~VulkanBufferResource()
{
    DestroyBuffer(*this);
}

VulkanImageResource::~VulkanImageResource()
{
    DestroyImage(*this);
}

VulkanResourceOwner::VulkanResourceOwner(VkPhysicalDevice physical, VkDevice device)
    : physical_(physical), device_(device)
{
}

VulkanResourceOwner::~VulkanResourceOwner() = default;

bool VulkanResourceOwner::FindMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags properties,
                                         std::uint32_t &index) const
{
    if (physical_ == VK_NULL_HANDLE)
        return false;

    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties);
    for (std::uint32_t candidate = 0; candidate < memoryProperties.memoryTypeCount; ++candidate)
    {
        if ((typeBits & (1u << candidate)) != 0 &&
            (memoryProperties.memoryTypes[candidate].propertyFlags & properties) == properties)
        {
            index = candidate;
            return true;
        }
    }
    return false;
}

VulkanResourceId VulkanResourceOwner::NextId()
{
    if (nextId_ == 0)
        return 0;
    const VulkanResourceId id = nextId_;
    ++nextId_;
    return id;
}

VulkanBufferCreateResult VulkanResourceOwner::CreateBuffer(const VulkanBufferDescriptor &descriptor)
{
    VulkanBufferCreateResult result;
    if (device_ == VK_NULL_HANDLE || descriptor.size == 0 || descriptor.usage == 0 ||
        (descriptor.map &&
         (descriptor.memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0))
        return result;

    auto resource = std::make_shared<VulkanBufferResource>();
    resource->device = device_;
    resource->size = descriptor.size;
    resource->usage = descriptor.usage;

    VkBufferCreateInfo createInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    createInfo.size = descriptor.size;
    createInfo.usage = descriptor.usage;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &createInfo, nullptr, &resource->buffer) != VK_SUCCESS)
    {
        result.status = VulkanResourceStatus::CreateFailed;
        return result;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, resource->buffer, &requirements);
    std::uint32_t memoryType = 0;
    if (!FindMemoryType(requirements.memoryTypeBits, descriptor.memoryProperties, memoryType))
    {
        result.status = VulkanResourceStatus::MemoryTypeUnavailable;
        return result;
    }

    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device_, &allocateInfo, nullptr, &resource->memory) != VK_SUCCESS)
    {
        result.status = VulkanResourceStatus::CreateFailed;
        return result;
    }
    if (vkBindBufferMemory(device_, resource->buffer, resource->memory, 0) != VK_SUCCESS)
    {
        result.status = VulkanResourceStatus::BindFailed;
        return result;
    }
    if (descriptor.map && vkMapMemory(device_, resource->memory, 0, descriptor.size, 0,
                                      &resource->mapped) != VK_SUCCESS)
    {
        result.status = VulkanResourceStatus::MapFailed;
        return result;
    }

    resource->id = NextId();
    if (resource->id == 0)
    {
        result.status = VulkanResourceStatus::CreateFailed;
        return result;
    }
    buffers_.emplace(resource->id, resource);
    result.status = VulkanResourceStatus::Accepted;
    result.resource = std::move(resource);
    return result;
}

VulkanImageCreateResult VulkanResourceOwner::CreateImage(const VulkanImageDescriptor &descriptor)
{
    VulkanImageCreateResult result;
    if (device_ == VK_NULL_HANDLE || descriptor.format == VK_FORMAT_UNDEFINED ||
        descriptor.extent.width == 0 || descriptor.extent.height == 0 || descriptor.usage == 0 ||
        descriptor.samples == 0 || descriptor.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
        return result;

    auto resource = std::make_shared<VulkanImageResource>();
    resource->device = device_;
    resource->aspectMask = descriptor.aspectMask;
    resource->resolve.format = descriptor.format;
    resource->resolve.extent = descriptor.extent;
    resource->resolve.samples = descriptor.samples;
    resource->resolve.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo createInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    createInfo.imageType = VK_IMAGE_TYPE_2D;
    createInfo.format = descriptor.format;
    createInfo.extent = {descriptor.extent.width, descriptor.extent.height, 1};
    createInfo.mipLevels = 1;
    createInfo.arrayLayers = 1;
    createInfo.samples = descriptor.samples;
    createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    createInfo.usage = descriptor.usage;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &createInfo, nullptr, &resource->resolve.image) != VK_SUCCESS)
    {
        result.status = VulkanResourceStatus::CreateFailed;
        return result;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, resource->resolve.image, &requirements);
    std::uint32_t memoryType = 0;
    if (!FindMemoryType(requirements.memoryTypeBits, descriptor.memoryProperties, memoryType))
    {
        result.status = VulkanResourceStatus::MemoryTypeUnavailable;
        return result;
    }

    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device_, &allocateInfo, nullptr, &resource->memory) != VK_SUCCESS)
    {
        result.status = VulkanResourceStatus::CreateFailed;
        return result;
    }
    if (vkBindImageMemory(device_, resource->resolve.image, resource->memory, 0) != VK_SUCCESS)
    {
        result.status = VulkanResourceStatus::BindFailed;
        return result;
    }

    resource->id = NextId();
    if (resource->id == 0)
    {
        result.status = VulkanResourceStatus::CreateFailed;
        return result;
    }
    images_.emplace(resource->id, resource);
    result.status = VulkanResourceStatus::Accepted;
    result.resource = std::move(resource);
    return result;
}

VulkanBufferLease VulkanResourceOwner::AcquireBuffer(VulkanResourceId id) const
{
    const auto found = buffers_.find(id);
    return found == buffers_.end() ? VulkanBufferLease{} : found->second;
}

VulkanImageLease VulkanResourceOwner::AcquireImage(VulkanResourceId id) const
{
    const auto found = images_.find(id);
    return found == images_.end() ? VulkanImageLease{} : found->second;
}

VulkanBufferLease VulkanResourceOwner::RetireBuffer(VulkanResourceId id)
{
    const auto found = buffers_.find(id);
    if (found == buffers_.end())
        return {};
    VulkanBufferLease resource = std::move(found->second);
    buffers_.erase(found);
    return resource;
}

VulkanImageLease VulkanResourceOwner::RetireImage(VulkanResourceId id)
{
    const auto found = images_.find(id);
    if (found == images_.end())
        return {};
    VulkanImageLease resource = std::move(found->second);
    images_.erase(found);
    return resource;
}

const char *VulkanResourceStatusText(VulkanResourceStatus status) noexcept
{
    switch (status)
    {
    case VulkanResourceStatus::Accepted:
        return "accepted";
    case VulkanResourceStatus::InvalidDescriptor:
        return "invalid native resource descriptor";
    case VulkanResourceStatus::MemoryTypeUnavailable:
        return "requested Vulkan memory properties are unavailable";
    case VulkanResourceStatus::CreateFailed:
        return "Vulkan resource creation failed";
    case VulkanResourceStatus::BindFailed:
        return "Vulkan resource memory binding failed";
    case VulkanResourceStatus::MapFailed:
        return "Vulkan resource mapping failed";
    }
    return "unknown";
}

} // namespace gears::native_rhi
