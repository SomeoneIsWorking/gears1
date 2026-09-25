#include "swapchain.h"

#include <algorithm>
#include <limits>

namespace gears::engine::render
{
namespace
{

// The surface format to present in: 8-bit BGRA or RGBA with sRGB encoding,
// else the first one the surface offers.
VkSurfaceFormatKHR ChooseFormat(VkPhysicalDevice physical, VkSurfaceKHR surface)
{
    std::uint32_t count = 0;
    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr),
          "vkGetPhysicalDeviceSurfaceFormatsKHR");
    std::vector<VkSurfaceFormatKHR> formats(count);
    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()),
          "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (formats.empty())
    {
        throw VulkanError("the window surface offers no formats");
    }
    auto srgb =
        std::ranges::find_if(formats,
                             [](const VkSurfaceFormatKHR &format)
                             {
                                 return (format.format == VK_FORMAT_B8G8R8A8_SRGB ||
                                         format.format == VK_FORMAT_R8G8B8A8_SRGB) &&
                                        format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                             });
    return srgb != formats.end() ? *srgb : formats.front();
}

VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR &capabilities, VkExtent2D drawable)
{
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max())
    {
        return capabilities.currentExtent;
    }
    return {std::clamp(drawable.width, capabilities.minImageExtent.width,
                       capabilities.maxImageExtent.width),
            std::clamp(drawable.height, capabilities.minImageExtent.height,
                       capabilities.maxImageExtent.height)};
}

} // namespace

Swapchain::Swapchain(const VulkanDevice &device, VkExtent2D drawable) : device_(device)
{
    if (device.Surface() == VK_NULL_HANDLE)
    {
        throw VulkanError("a swapchain needs a device created with a window surface");
    }
    Create(drawable, VK_NULL_HANDLE);
}

Swapchain::~Swapchain()
{
    vkDestroySwapchainKHR(device_.Device(), swapchain_, nullptr);
}

void Swapchain::Recreate(VkExtent2D drawable)
{
    Check(vkDeviceWaitIdle(device_.Device()), "vkDeviceWaitIdle");
    VkSwapchainKHR previous = swapchain_;
    Create(drawable, previous);
    vkDestroySwapchainKHR(device_.Device(), previous, nullptr);
}

void Swapchain::Create(VkExtent2D drawable, VkSwapchainKHR previous)
{
    VkSurfaceCapabilitiesKHR capabilities{};
    Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.Physical(), device_.Surface(),
                                                    &capabilities),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkSurfaceFormatKHR format = ChooseFormat(device_.Physical(), device_.Surface());
    std::uint32_t image_count = capabilities.minImageCount + 1U;
    if (capabilities.maxImageCount != 0U)
    {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }
    extent_ = ChooseExtent(capabilities, drawable);

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = device_.Surface();
    info.minImageCount = image_count;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO is the one present mode every surface supports.
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = previous;
    Check(vkCreateSwapchainKHR(device_.Device(), &info, nullptr, &swapchain_),
          "vkCreateSwapchainKHR");

    std::uint32_t count = 0;
    Check(vkGetSwapchainImagesKHR(device_.Device(), swapchain_, &count, nullptr),
          "vkGetSwapchainImagesKHR");
    images_.resize(count);
    Check(vkGetSwapchainImagesKHR(device_.Device(), swapchain_, &count, images_.data()),
          "vkGetSwapchainImagesKHR");
}

std::optional<std::uint32_t> Swapchain::Acquire(VkSemaphore ready)
{
    std::uint32_t index = 0;
    VkResult result = vkAcquireNextImageKHR(device_.Device(), swapchain_,
                                            std::numeric_limits<std::uint64_t>::max(), ready,
                                            VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return std::nullopt;
    }
    if (result != VK_SUBOPTIMAL_KHR)
    {
        Check(result, "vkAcquireNextImageKHR");
    }
    return index;
}

bool Swapchain::Present(std::uint32_t image, VkSemaphore rendered)
{
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image;
    VkResult result = vkQueuePresentKHR(device_.Queue(), &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        return false;
    }
    Check(result, "vkQueuePresentKHR");
    return true;
}

} // namespace gears::engine::render
