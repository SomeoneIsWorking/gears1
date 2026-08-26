#include "gpu_present_source.h"

#include <utility>

namespace gears
{

bool PresentSourceSlots::Begin(VkDevice device, VkFence fence, size_t slot)
{
    if (slot >= kCapacity)
        return false;
    if (!submitted_[slot])
        return true;
    if (vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return false;
    Release(slot);
    return true;
}

PresentSourceSlots::RecordedFrame
PresentSourceSlots::RecordLatest(VkCommandBuffer commands, VkImage destination, VkExtent2D extent,
                                 bool swapchainIsSrgb, SrgbRawCopyStage &srgbStage)
{
    SharedFrameImage frame;
    if (!AcquireSharedFrameImage(frame) || frame.width != extent.width ||
        frame.height != extent.height)
        return {};

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(frame.width), int32_t(frame.height), 1};
    blit.dstOffsets[1] = {int32_t(extent.width), int32_t(extent.height), 1};
    if (swapchainIsSrgb && srgbStage.Image() != VK_NULL_HANDLE)
    {
        VkImageSubresourceRange one{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier toDestination{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDestination.srcAccessMask = 0;
        toDestination.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDestination.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDestination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDestination.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDestination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDestination.image = srgbStage.Image();
        toDestination.subresourceRange = one;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toDestination);
        vkCmdBlitImage(commands, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       srgbStage.Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_NEAREST);
        VkImageMemoryBarrier toSource = toDestination;
        toSource.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSource.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toSource);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {extent.width, extent.height, 1};
        vkCmdCopyImage(commands, srgbStage.Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    else
    {
        vkCmdBlitImage(commands, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }
    return {std::move(frame.lease), frame.sequence, true};
}

void PresentSourceSlots::Submitted(size_t slot, SharedFrameImage::Lease lease)
{
    if (slot >= kCapacity)
        return;
    leases_[slot] = std::move(lease);
    submitted_[slot] = true;
}

void PresentSourceSlots::Release(size_t slot)
{
    if (slot >= kCapacity)
        return;
    leases_[slot].Reset();
    submitted_[slot] = false;
}

void PresentSourceSlots::ResetAfterDeviceIdle()
{
    for (size_t slot = 0; slot < kCapacity; ++slot)
        Release(slot);
}

} // namespace gears
