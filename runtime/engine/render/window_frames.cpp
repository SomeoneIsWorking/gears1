#include "window_frames.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace gears::engine::render
{
namespace
{

// The colour outside the letterboxed frame.
constexpr VkClearColorValue kLetterbox{{0.0F, 0.0F, 0.0F, 1.0F}};

void Transition(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to,
                VkAccessFlags from_access, VkAccessFlags to_access, VkPipelineStageFlags from_stage,
                VkPipelineStageFlags to_stage)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = from_access;
    barrier.dstAccessMask = to_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, from_stage, to_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// The largest rectangle of `source`'s aspect centred in `target`.
std::array<VkOffset3D, 2> Letterbox(VkExtent2D source, VkExtent2D target)
{
    double scale = std::min(static_cast<double>(target.width) / source.width,
                            static_cast<double>(target.height) / source.height);
    auto width = static_cast<std::int32_t>(source.width * scale);
    auto height = static_cast<std::int32_t>(source.height * scale);
    std::int32_t x = (static_cast<std::int32_t>(target.width) - width) / 2;
    std::int32_t y = (static_cast<std::int32_t>(target.height) - height) / 2;
    return {VkOffset3D{x, y, 0}, VkOffset3D{x + width, y + height, 1}};
}

} // namespace

WindowFrames::WindowFrames(const VulkanDevice &device, VkExtent2D drawable)
    : device_(device), swapchain_(device, drawable)
{
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = device.CommandPool();
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(device.Device(), &allocate, &commands_),
          "vkAllocateCommandBuffers");
    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    Check(vkCreateSemaphore(device.Device(), &semaphore, nullptr, &image_ready_),
          "vkCreateSemaphore");
    Check(vkCreateSemaphore(device.Device(), &semaphore, nullptr, &rendered_), "vkCreateSemaphore");
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    Check(vkCreateFence(device.Device(), &fence, nullptr, &finished_), "vkCreateFence");
}

WindowFrames::~WindowFrames()
{
    vkDeviceWaitIdle(device_.Device());
    vkDestroyFence(device_.Device(), finished_, nullptr);
    vkDestroySemaphore(device_.Device(), rendered_, nullptr);
    vkDestroySemaphore(device_.Device(), image_ready_, nullptr);
    vkFreeCommandBuffers(device_.Device(), device_.CommandPool(), 1, &commands_);
}

void WindowFrames::Draw(VkExtent2D drawable, const std::function<void(VkCommandBuffer)> &render,
                        VkImage source, VkExtent2D source_extent)
{
    Check(vkWaitForFences(device_.Device(), 1, &finished_, VK_TRUE,
                          std::numeric_limits<std::uint64_t>::max()),
          "vkWaitForFences");
    if (drawable.width == 0U || drawable.height == 0U)
    {
        return;
    }
    std::optional<std::uint32_t> image = swapchain_.Acquire(image_ready_);
    if (!image)
    {
        swapchain_.Recreate(drawable);
        return;
    }
    Check(vkResetFences(device_.Device(), 1, &finished_), "vkResetFences");
    Check(vkResetCommandBuffer(commands_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(commands_, &begin), "vkBeginCommandBuffer");
    render(commands_);
    RecordPresentation(source, source_extent, swapchain_.Image(*image));
    Check(vkEndCommandBuffer(commands_), "vkEndCommandBuffer");

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_ready_;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &rendered_;
    Check(vkQueueSubmit(device_.Queue(), 1, &submit, finished_), "vkQueueSubmit");
    VkExtent2D current = swapchain_.Extent();
    if (!swapchain_.Present(*image, rendered_) || current.width != drawable.width ||
        current.height != drawable.height)
    {
        swapchain_.Recreate(drawable);
    }
}

void WindowFrames::RecordPresentation(VkImage source, VkExtent2D source_extent,
                                      VkImage target) const
{
    // The frame's colour writes finish before the blit reads them.
    Transition(commands_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    Transition(commands_, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(commands_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &kLetterbox, 1,
                         &whole);
    Transition(commands_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<std::int32_t>(source_extent.width),
                          static_cast<std::int32_t>(source_extent.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    std::array<VkOffset3D, 2> destination = Letterbox(source_extent, swapchain_.Extent());
    blit.dstOffsets[0] = destination[0];
    blit.dstOffsets[1] = destination[1];
    vkCmdBlitImage(commands_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    Transition(commands_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

} // namespace gears::engine::render
