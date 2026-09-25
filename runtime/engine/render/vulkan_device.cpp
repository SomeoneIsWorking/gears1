#include "vulkan_device.h"

#include <array>
#include <format>
#include <utility>
#include <vector>

namespace gears::engine::render
{

void Check(VkResult result, const char *call)
{
    if (result != VK_SUCCESS)
    {
        throw VulkanError(
            std::format("{} failed with VkResult {}", call, static_cast<int>(result)));
    }
}

std::uint32_t FindMemoryType(VkPhysicalDevice physical, std::uint32_t allowed,
                             VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
    {
        if ((allowed & (1U << i)) != 0U &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    throw VulkanError("no memory type has the required properties");
}

HostBuffer::HostBuffer(VkDevice device, VkPhysicalDevice physical, VkDeviceSize size,
                       VkBufferUsageFlags usage)
    : device_(device), size_(static_cast<std::size_t>(size))
{
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateBuffer(device_, &info, nullptr, &buffer_), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer_, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        FindMemoryType(physical, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(device_, &allocate, nullptr, &memory_), "vkAllocateMemory");
    Check(vkBindBufferMemory(device_, buffer_, memory_, 0), "vkBindBufferMemory");
    void *mapped = nullptr;
    Check(vkMapMemory(device_, memory_, 0, size, 0, &mapped), "vkMapMemory");
    mapped_ = static_cast<std::uint8_t *>(mapped);
}

HostBuffer::HostBuffer(HostBuffer &&other) noexcept
    : device_(other.device_), buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      memory_(std::exchange(other.memory_, VK_NULL_HANDLE)),
      mapped_(std::exchange(other.mapped_, nullptr)), size_(other.size_)
{
}

HostBuffer::~HostBuffer()
{
    if (buffer_ != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(device_, buffer_, nullptr);
    }
    if (memory_ != VK_NULL_HANDLE)
    {
        vkFreeMemory(device_, memory_, nullptr);
    }
}

VulkanDevice::VulkanDevice(std::optional<SurfaceRequest> surface)
{
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "GearsUE3";
    application.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    if (surface)
    {
        instance_info.enabledExtensionCount =
            static_cast<std::uint32_t>(surface->instance_extensions.size());
        instance_info.ppEnabledExtensionNames = surface->instance_extensions.data();
    }
    Check(vkCreateInstance(&instance_info, nullptr, &instance_), "vkCreateInstance");
    if (surface)
    {
        surface_ = surface->create_surface(instance_);
        if (surface_ == VK_NULL_HANDLE)
        {
            vkDestroyInstance(instance_, nullptr);
            throw VulkanError("the window system could not create a Vulkan surface");
        }
    }
    ChooseDevice();
    CreateLogicalDevice();
}

void VulkanDevice::ChooseDevice()
{
    std::uint32_t count = 0;
    Check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()),
          "vkEnumeratePhysicalDevices");
    bool chosen_discrete = false;
    for (VkPhysicalDevice candidate : devices)
    {
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        for (std::uint32_t family = 0; family < family_count; ++family)
        {
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U)
            {
                continue;
            }
            if (surface_ != VK_NULL_HANDLE)
            {
                VkBool32 presents = VK_FALSE;
                Check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface_, &presents),
                      "vkGetPhysicalDeviceSurfaceSupportKHR");
                if (presents == VK_FALSE)
                {
                    continue;
                }
            }
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            bool discrete = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (physical_ == VK_NULL_HANDLE || (discrete && !chosen_discrete))
            {
                physical_ = candidate;
                queue_family_ = family;
                chosen_discrete = discrete;
                name_ = properties.deviceName;
            }
            break;
        }
    }
    if (physical_ == VK_NULL_HANDLE)
    {
        if (surface_ != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        }
        vkDestroyInstance(instance_, nullptr);
        throw VulkanError(surface_ != VK_NULL_HANDLE
                              ? "no Vulkan device has a graphics queue that presents to the window"
                              : "no Vulkan device has a graphics queue");
    }
}

void VulkanDevice::CreateLogicalDevice()
{
    float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    std::array<const char *, 1> swapchain_extension{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    if (surface_ != VK_NULL_HANDLE)
    {
        device_info.enabledExtensionCount = static_cast<std::uint32_t>(swapchain_extension.size());
        device_info.ppEnabledExtensionNames = swapchain_extension.data();
    }
    Check(vkCreateDevice(physical_, &device_info, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_;
    Check(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_), "vkCreateCommandPool");
}

VulkanDevice::~VulkanDevice()
{
    vkDeviceWaitIdle(device_);
    vkDestroyCommandPool(device_, command_pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    if (surface_ != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    vkDestroyInstance(instance_, nullptr);
}

std::uint32_t VulkanDevice::MemoryType(std::uint32_t allowed,
                                       VkMemoryPropertyFlags properties) const
{
    return FindMemoryType(physical_, allowed, properties);
}

VkCommandBuffer VulkanDevice::BeginOneTime() const
{
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = command_pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    Check(vkAllocateCommandBuffers(device_, &allocate, &commands), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
    return commands;
}

void VulkanDevice::EndOneTime(VkCommandBuffer commands) const
{
    Check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    Check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    Check(vkQueueWaitIdle(queue_), "vkQueueWaitIdle");
    vkFreeCommandBuffers(device_, command_pool_, 1, &commands);
}

} // namespace gears::engine::render
