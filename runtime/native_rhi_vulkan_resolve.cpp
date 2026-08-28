#include "native_rhi_vulkan_resolve.h"

#include <cstdint>

namespace gears::native_rhi
{
namespace
{

struct Rect
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

[[nodiscard]] bool PositiveAndInside(const Rect &rect, VkExtent2D extent)
{
    const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
    const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
    return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 &&
           right <= extent.width && bottom <= extent.height;
}

[[nodiscard]] VkAccessFlags AccessForLayout(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        return 0;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return VK_ACCESS_SHADER_READ_BIT;
    default:
        return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
}

[[nodiscard]] std::uint32_t BytesPerPixel(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R32_SFLOAT:
        return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R32G32_SFLOAT:
        return 8;
    default:
        return 0;
    }
}

void Transition(VkCommandBuffer commands, const VulkanResolveImage &image, VkImageLayout newLayout,
                VkAccessFlags destinationAccess)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = AccessForLayout(image.layout);
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = image.layout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

VulkanResolveStatus RecordColorResolve(VkCommandBuffer commands, const RhiSemanticResolve &resolve,
                                       VulkanResolveImage &source, VulkanResolveImage &destination)
{
    if (commands == VK_NULL_HANDLE || source.image == VK_NULL_HANDLE ||
        destination.image == VK_NULL_HANDLE)
        return VulkanResolveStatus::MissingImage;
    if (source.image == destination.image)
        return VulkanResolveStatus::SameImage;
    if (resolve.sourceDepthStencil)
        return VulkanResolveStatus::DepthStencilSource;
    if ((resolve.operationFlags & ~7u) != 0)
        return VulkanResolveStatus::UnsupportedOperationFlags;
    if (resolve.sourceSlot != (resolve.operationFlags & 7u) || resolve.sourceSlot > 3)
        return VulkanResolveStatus::InvalidSourceSelection;

    const Rect sourceRect{resolve.sourceRectangle[0], resolve.sourceRectangle[1],
                          resolve.sourceRectangle[2], resolve.sourceRectangle[3]};
    if (!PositiveAndInside(sourceRect, source.extent))
        return VulkanResolveStatus::InvalidSourceRectangle;

    const std::int32_t destinationX = resolve.destinationPoint[0];
    const std::int32_t destinationY = resolve.destinationPoint[1];
    const std::int64_t destinationRight =
        static_cast<std::int64_t>(destinationX) + sourceRect.width;
    const std::int64_t destinationBottom =
        static_cast<std::int64_t>(destinationY) + sourceRect.height;
    if (destinationX < 0 || destinationY < 0 || destinationRight > destination.extent.width ||
        destinationBottom > destination.extent.height)
        return VulkanResolveStatus::InvalidDestinationPoint;
    if (resolve.destinationPitch < static_cast<std::uint32_t>(destinationRight))
        return VulkanResolveStatus::InvalidDestinationPitch;
    if (resolve.destinationHeight < static_cast<std::uint32_t>(sourceRect.height))
        return VulkanResolveStatus::InvalidDestinationHeight;
    if (source.format == VK_FORMAT_UNDEFINED || source.format != destination.format)
        return VulkanResolveStatus::FormatMismatch;
    const std::uint32_t bytesPerPixel = BytesPerPixel(source.format);
    if (bytesPerPixel == 0)
        return VulkanResolveStatus::UnsupportedFormat;
    if (resolve.bytesPerBlock != bytesPerPixel)
        return VulkanResolveStatus::BytesPerPixelMismatch;
    if (source.samples != VK_SAMPLE_COUNT_1_BIT || destination.samples != VK_SAMPLE_COUNT_1_BIT)
        return VulkanResolveStatus::MultisampleUnsupported;

    Transition(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
    Transition(commands, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT);

    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.srcOffset = {sourceRect.x, sourceRect.y, 0};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstOffset = {destinationX, destinationY, 0};
    copy.extent = {static_cast<std::uint32_t>(sourceRect.width),
                   static_cast<std::uint32_t>(sourceRect.height), 1};
    vkCmdCopyImage(commands, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    source.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    destination.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    return VulkanResolveStatus::Accepted;
}

const char *VulkanResolveStatusText(VulkanResolveStatus status) noexcept
{
    switch (status)
    {
    case VulkanResolveStatus::Accepted:
        return "accepted";
    case VulkanResolveStatus::MissingImage:
        return "missing image or command buffer";
    case VulkanResolveStatus::SameImage:
        return "source and destination image are the same";
    case VulkanResolveStatus::DepthStencilSource:
        return "depth/stencil source is not a colour copy";
    case VulkanResolveStatus::UnsupportedOperationFlags:
        return "resolve operation flags are not grounded for a native copy";
    case VulkanResolveStatus::InvalidSourceSelection:
        return "source selection does not match the native colour-copy class";
    case VulkanResolveStatus::InvalidSourceRectangle:
        return "source rectangle is outside the host image";
    case VulkanResolveStatus::InvalidDestinationPoint:
        return "destination rectangle is outside the host image";
    case VulkanResolveStatus::InvalidDestinationPitch:
        return "destination pitch cannot contain the copied rectangle";
    case VulkanResolveStatus::InvalidDestinationHeight:
        return "destination height cannot contain the copied rectangle";
    case VulkanResolveStatus::FormatMismatch:
        return "native source and destination formats differ";
    case VulkanResolveStatus::UnsupportedFormat:
        return "native colour format is not in the exact-copy class";
    case VulkanResolveStatus::BytesPerPixelMismatch:
        return "resolve byte width does not match the native colour format";
    case VulkanResolveStatus::MultisampleUnsupported:
        return "multisample resolve semantics are not a one-to-one copy";
    }
    return "unknown";
}

} // namespace gears::native_rhi
