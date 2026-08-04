// Choosing the swapchain's format.
//
// This is a pure function over the list the surface reports, in its own header,
// because getting it wrong is invisible to every capture this project takes and
// visible on every pixel of the user's screen.
//
// The frame handed to the presenter is R8G8B8A8_UNORM holding bytes the guest has
// already tonemapped -- display-ready values, not linear light. vkCmdBlitImage
// between formats CONVERTS, so blitting those bytes into a *_SRGB swapchain image
// makes the driver treat them as linear and encode them: mid-tones lift hard,
// blacks stay black, and the window shows a flat washed-out version of a frame the
// renderer produced correctly (catalog #60).
//
// The old selection preferred B8G8R8A8_UNORM with SRGB_NONLINEAR and fell back to
// formats[0] otherwise -- and formats[0] is B8G8R8A8_SRGB on the driver this was
// found on. The fallback was the bug; the preference was fine.
#pragma once

#include <cstddef>

#include <vulkan/vulkan.h>

namespace gears
{

inline bool SwapchainFormatIsSrgb(VkFormat f)
{
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB ||
           f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

inline bool SwapchainFormatIsUnorm(VkFormat f)
{
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM ||
           f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
           f == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
}

// Picks in this order: B8G8R8A8_UNORM with the usual colour space, any other UNORM,
// any non-sRGB format, and only then whatever came first. The caller reports which
// it got, and warns when the answer is an sRGB one -- a surface offering nothing
// else is a real possibility and the user should be told their window will look
// wrong rather than left to wonder.
inline VkSurfaceFormatKHR ChooseSwapchainFormat(const VkSurfaceFormatKHR* formats,
                                                size_t count)
{
    if (formats == nullptr || count == 0)
        return VkSurfaceFormatKHR{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};

    VkSurfaceFormatKHR chosen = formats[0];
    bool foundUnorm = false;
    for (size_t i = 0; i < count; ++i)
    {
        const VkSurfaceFormatKHR& candidate = formats[i];
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM &&
            candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return candidate;
        if (!foundUnorm && SwapchainFormatIsUnorm(candidate.format))
        {
            chosen = candidate;
            foundUnorm = true;
        }
    }
    if (foundUnorm)
        return chosen;
    for (size_t i = 0; i < count; ++i)
        if (!SwapchainFormatIsSrgb(formats[i].format))
            return formats[i];
    return formats[0];
}

} // namespace gears
