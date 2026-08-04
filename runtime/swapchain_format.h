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

// THE COLOUR SPACE IS PART OF THE ANSWER, not a detail of it.
//
// A UNORM format paired with EXTENDED_SRGB_LINEAR or HDR10_ST2084 tells the
// compositor to read the frame's bytes as linear light or as PQ. Those bytes are
// sRGB-encoded, so the display maps them for a range they were never in: mid-tones
// lift, contrast flattens, and the window looks washed out while every pixel handed
// to the swapchain is byte-perfect. A desktop in HDR mode offers exactly those
// pairings, and an earlier version of this function preferred ANY UNORM over the
// colour space -- which on such a desktop picks the wrong one.
//
// So SRGB_NONLINEAR comes first, and the format second:
//   1. B8G8R8A8_UNORM with SRGB_NONLINEAR -- what a normal desktop offers
//   2. any UNORM with SRGB_NONLINEAR
//   3. any non-sRGB format with SRGB_NONLINEAR
//   4. any UNORM at all (a surface with no sRGB_NONLINEAR entry)
//   5. any non-sRGB format
//   6. whatever came first, and the caller warns
inline VkSurfaceFormatKHR ChooseSwapchainFormat(const VkSurfaceFormatKHR* formats,
                                                size_t count)
{
    if (formats == nullptr || count == 0)
        return VkSurfaceFormatKHR{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};

    auto find = [&](bool wantSrgbSpace, bool wantUnorm) -> const VkSurfaceFormatKHR* {
        for (size_t i = 0; i < count; ++i)
        {
            const VkSurfaceFormatKHR& c = formats[i];
            if (wantSrgbSpace && c.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                continue;
            if (wantUnorm ? !SwapchainFormatIsUnorm(c.format)
                          : SwapchainFormatIsSrgb(c.format))
                continue;
            return &c;
        }
        return nullptr;
    };

    for (size_t i = 0; i < count; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return formats[i];
    if (const VkSurfaceFormatKHR* c = find(true, true))   return *c;
    if (const VkSurfaceFormatKHR* c = find(true, false))  return *c;
    if (const VkSurfaceFormatKHR* c = find(false, true))  return *c;
    if (const VkSurfaceFormatKHR* c = find(false, false)) return *c;
    return formats[0];
}

} // namespace gears
