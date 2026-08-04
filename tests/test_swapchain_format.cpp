// Tests for the swapchain format choice.
//
// The property under test decides what every pixel of the window looks like and is
// invisible to every capture this project takes: the renderer's screenshots come
// from its own readback, before the blit into the swapchain. An sRGB swapchain
// makes that blit re-encode an already-tonemapped frame, and the window shows a
// flat washed-out picture while every file on disk looks correct (catalog #60).
//
// The first case is the real driver's list, in the real order, with the sRGB format
// first -- which is what the old fallback picked.

#include <cstdio>
#include <vector>

#include "swapchain_format.h"

namespace
{

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

constexpr VkColorSpaceKHR kSrgbNonlinear = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

using gears::ChooseSwapchainFormat;

// THE CASE THAT SHIPPED WRONG.
void TestSrgbFirstIsNotChosenWhenUnormExists()
{
    const std::vector<VkSurfaceFormatKHR> formats{
        {VK_FORMAT_B8G8R8A8_SRGB, kSrgbNonlinear},
        {VK_FORMAT_B8G8R8A8_UNORM, kSrgbNonlinear},
        {VK_FORMAT_R8G8B8A8_SRGB, kSrgbNonlinear},
    };
    const VkSurfaceFormatKHR got = ChooseSwapchainFormat(formats.data(), formats.size());
    Check(got.format == VK_FORMAT_B8G8R8A8_UNORM,
        "a surface listing sRGB FIRST and UNORM second gives the UNORM -- the old"
        " code took formats[0] and washed out the whole window");
    Check(!gears::SwapchainFormatIsSrgb(got.format),
        "and the chosen format is not an sRGB one");
}

void TestAnyUnormBeatsAnySrgb()
{
    // No B8G8R8A8_UNORM at all: the next UNORM must still win over the sRGB ones.
    const std::vector<VkSurfaceFormatKHR> formats{
        {VK_FORMAT_R8G8B8A8_SRGB, kSrgbNonlinear},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, kSrgbNonlinear},
    };
    const VkSurfaceFormatKHR got = ChooseSwapchainFormat(formats.data(), formats.size());
    Check(got.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        "a ten-bit UNORM is preferred over an eight-bit sRGB");
}

void TestNonSrgbBeatsSrgbEvenWhenNotUnorm()
{
    const std::vector<VkSurfaceFormatKHR> formats{
        {VK_FORMAT_B8G8R8A8_SRGB, kSrgbNonlinear},
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
    };
    const VkSurfaceFormatKHR got = ChooseSwapchainFormat(formats.data(), formats.size());
    Check(!gears::SwapchainFormatIsSrgb(got.format),
        "with no UNORM offered, any non-sRGB format is still preferred to an sRGB one");
}

// A surface really can offer nothing else. The choice is then forced, and the
// caller's job is to SAY so -- but this function must not pretend otherwise.
void TestSrgbOnlySurfaceIsReportedHonestly()
{
    const std::vector<VkSurfaceFormatKHR> formats{
        {VK_FORMAT_B8G8R8A8_SRGB, kSrgbNonlinear},
    };
    const VkSurfaceFormatKHR got = ChooseSwapchainFormat(formats.data(), formats.size());
    Check(got.format == VK_FORMAT_B8G8R8A8_SRGB,
        "an sRGB-only surface yields the sRGB format rather than an invalid one");
    Check(gears::SwapchainFormatIsSrgb(got.format),
        "and it is recognisable as sRGB, which is what makes the caller warn");
}

void TestEmptyListDoesNotInventAFormat()
{
    const VkSurfaceFormatKHR got = ChooseSwapchainFormat(nullptr, 0);
    Check(got.format == VK_FORMAT_UNDEFINED,
        "no formats yields UNDEFINED rather than a plausible guess");
}

// AN HDR DESKTOP. The surface offers UNORM formats, but paired with colour spaces
// that tell the compositor the bytes are linear light or PQ. Taking one of those
// because it is UNORM re-interprets an sRGB-encoded frame and washes the window
// out with every pixel in the swapchain byte-perfect.
void TestColourSpaceBeatsFormatPreference()
{
    const std::vector<VkSurfaceFormatKHR> formats{
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
        {VK_FORMAT_B8G8R8A8_SRGB, kSrgbNonlinear},
        {VK_FORMAT_B8G8R8A8_UNORM, kSrgbNonlinear},
    };
    const VkSurfaceFormatKHR got = ChooseSwapchainFormat(formats.data(), formats.size());
    Check(got.format == VK_FORMAT_B8G8R8A8_UNORM &&
          got.colorSpace == kSrgbNonlinear,
        "with HDR colour spaces listed FIRST, the sRGB-nonlinear UNORM still wins --"
        " a linear or PQ colour space re-interprets an already-encoded frame");
}

void TestSrgbNonlinearWinsEvenWithoutAUnormFormat()
{
    const std::vector<VkSurfaceFormatKHR> formats{
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, kSrgbNonlinear},
    };
    const VkSurfaceFormatKHR got = ChooseSwapchainFormat(formats.data(), formats.size());
    Check(got.colorSpace == kSrgbNonlinear,
        "a UNORM with SRGB_NONLINEAR is preferred to a float format in a linear space");
}

} // namespace

int main()
{
    TestSrgbFirstIsNotChosenWhenUnormExists();
    TestColourSpaceBeatsFormatPreference();
    TestSrgbNonlinearWinsEvenWithoutAUnormFormat();
    TestAnyUnormBeatsAnySrgb();
    TestNonSrgbBeatsSrgbEvenWhenNotUnorm();
    TestSrgbOnlySurfaceIsReportedHonestly();
    TestEmptyListDoesNotInventAFormat();
    if (g_failures != 0)
    {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("swapchain format: all checks passed\n");
    return 0;
}
