#pragma once

#include "native_rhi_vulkan_resolve.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

#include <vulkan/vulkan.h>

namespace gears::native_rhi
{

using VulkanResourceId = std::uint64_t;

enum class VulkanResourceStatus : std::uint8_t
{
    Accepted,
    InvalidDescriptor,
    MemoryTypeUnavailable,
    CreateFailed,
    BindFailed,
    MapFailed,
};

struct VulkanBufferResource
{
    VulkanResourceId id = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    void *mapped = nullptr;

    ~VulkanBufferResource();
};

struct VulkanImageResource
{
    VulkanResourceId id = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageAspectFlags aspectMask = 0;
    VulkanResolveImage resolve;

    ~VulkanImageResource();
};

using VulkanBufferLease = std::shared_ptr<VulkanBufferResource>;
using VulkanImageLease = std::shared_ptr<VulkanImageResource>;

struct VulkanBufferDescriptor
{
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    VkMemoryPropertyFlags memoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    bool map = false;
};

struct VulkanImageDescriptor
{
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkMemoryPropertyFlags memoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
};

struct VulkanBufferCreateResult
{
    VulkanResourceStatus status = VulkanResourceStatus::InvalidDescriptor;
    VulkanBufferLease resource;
};

struct VulkanImageCreateResult
{
    VulkanResourceStatus status = VulkanResourceStatus::InvalidDescriptor;
    VulkanImageLease resource;
};

// Owns host Vulkan allocations for the native path. Guest object numbers and
// construction words never enter this API: a caller supplies a complete host
// descriptor, receives an explicit native ID, and retains the returned lease
// until the submission fence has completed.
class VulkanResourceOwner
{
  public:
    VulkanResourceOwner(VkPhysicalDevice physical, VkDevice device);
    ~VulkanResourceOwner();

    VulkanResourceOwner(const VulkanResourceOwner &) = delete;
    VulkanResourceOwner &operator=(const VulkanResourceOwner &) = delete;

    [[nodiscard]] VulkanBufferCreateResult CreateBuffer(const VulkanBufferDescriptor &descriptor);
    [[nodiscard]] VulkanImageCreateResult CreateImage(const VulkanImageDescriptor &descriptor);

    [[nodiscard]] VulkanBufferLease AcquireBuffer(VulkanResourceId id) const;
    [[nodiscard]] VulkanImageLease AcquireImage(VulkanResourceId id) const;

    // Removes the active lookup entry and returns the lease that must be held
    // through GPU completion. Destruction therefore cannot race a submitted
    // command buffer, and an unknown ID refuses instead of being ignored.
    [[nodiscard]] VulkanBufferLease RetireBuffer(VulkanResourceId id);
    [[nodiscard]] VulkanImageLease RetireImage(VulkanResourceId id);

    [[nodiscard]] std::size_t ActiveBufferCount() const noexcept { return buffers_.size(); }
    [[nodiscard]] std::size_t ActiveImageCount() const noexcept { return images_.size(); }

  private:
    [[nodiscard]] bool FindMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags properties,
                                      std::uint32_t &index) const;
    [[nodiscard]] VulkanResourceId NextId();

    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VulkanResourceId nextId_ = 1;
    std::map<VulkanResourceId, VulkanBufferLease> buffers_;
    std::map<VulkanResourceId, VulkanImageLease> images_;
};

[[nodiscard]] const char *VulkanResourceStatusText(VulkanResourceStatus status) noexcept;

} // namespace gears::native_rhi
