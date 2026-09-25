#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace gears::engine::render
{

// A Vulkan call that failed; names the call and its result code.
class VulkanError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

void Check(VkResult result, const char *call);

// A buffer and its host-visible memory, mapped for the buffer's lifetime.
class HostBuffer
{
  public:
    HostBuffer(VkDevice device, VkPhysicalDevice physical, VkDeviceSize size,
               VkBufferUsageFlags usage);
    ~HostBuffer();
    HostBuffer(const HostBuffer &) = delete;
    HostBuffer &operator=(const HostBuffer &) = delete;
    HostBuffer(HostBuffer &&other) noexcept;
    HostBuffer &operator=(HostBuffer &&) = delete;

    [[nodiscard]] VkBuffer Handle() const noexcept { return buffer_; }
    [[nodiscard]] std::span<std::uint8_t> Bytes() noexcept { return {mapped_, size_}; }

  private:
    VkDevice device_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    std::uint8_t *mapped_ = nullptr;
    std::size_t size_;
};

// What a device that presents to a window needs: the instance extensions
// the window system requires, and how to create the window's surface once
// the instance exists.
struct SurfaceRequest
{
    std::vector<const char *> instance_extensions;
    std::function<VkSurfaceKHR(VkInstance)> create_surface;
};

// One Vulkan device: instance, the chosen physical device, a logical device
// with one graphics queue, and a command pool on it; with a surface request,
// also the window's surface, a queue that presents to it, and swapchains.
class VulkanDevice
{
  public:
    // Chooses the first discrete GPU, else the first device whose graphics
    // queue can present to the surface when one is requested. Refuses when
    // there is none.
    explicit VulkanDevice(std::optional<SurfaceRequest> surface = std::nullopt);
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice &) = delete;
    VulkanDevice &operator=(const VulkanDevice &) = delete;

    [[nodiscard]] VkInstance Instance() const noexcept { return instance_; }
    [[nodiscard]] VkDevice Device() const noexcept { return device_; }
    // The window's surface; VK_NULL_HANDLE for a headless device.
    [[nodiscard]] VkSurfaceKHR Surface() const noexcept { return surface_; }
    [[nodiscard]] std::uint32_t QueueFamily() const noexcept { return queue_family_; }
    [[nodiscard]] VkPhysicalDevice Physical() const noexcept { return physical_; }
    [[nodiscard]] VkQueue Queue() const noexcept { return queue_; }
    [[nodiscard]] VkCommandPool CommandPool() const noexcept { return command_pool_; }
    [[nodiscard]] const std::string &Name() const noexcept { return name_; }

    [[nodiscard]] std::uint32_t MemoryType(std::uint32_t allowed,
                                           VkMemoryPropertyFlags properties) const;
    [[nodiscard]] HostBuffer CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage) const
    {
        return {device_, physical_, size, usage};
    }

    // Records commands through `record` into a one-time command buffer,
    // submits it, and waits for the queue to finish.
    template <typename Record> void Submit(Record record) const
    {
        VkCommandBuffer commands = BeginOneTime();
        record(commands);
        EndOneTime(commands);
    }

  private:
    [[nodiscard]] VkCommandBuffer BeginOneTime() const;
    void EndOneTime(VkCommandBuffer commands) const;

    // Picks the physical device and its queue family.
    void ChooseDevice();
    void CreateLogicalDevice();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::string name_;
};

std::uint32_t FindMemoryType(VkPhysicalDevice physical, std::uint32_t allowed,
                             VkMemoryPropertyFlags properties);

} // namespace gears::engine::render
