#include "offscreen_target.h"

#include <array>

namespace gears::engine::render
{

DeviceImage::DeviceImage(const VulkanDevice &device, VkFormat format, VkExtent2D extent,
                         VkImageUsageFlags usage, VkImageAspectFlags aspect)
    : device_(device.Device())
{
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(device_, &info, nullptr, &image_), "vkCreateImage");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image_, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        device.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device_, &allocate, nullptr, &memory_), "vkAllocateMemory");
    Check(vkBindImageMemory(device_, image_, memory_, 0), "vkBindImageMemory");
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    Check(vkCreateImageView(device_, &view, nullptr, &view_), "vkCreateImageView");
}

DeviceImage::~DeviceImage()
{
    vkDestroyImageView(device_, view_, nullptr);
    vkDestroyImage(device_, image_, nullptr);
    vkFreeMemory(device_, memory_, nullptr);
}

OffscreenTarget::OffscreenTarget(const VulkanDevice &device, VkExtent2D extent)
    : device_(device), extent_(extent),
      color_(device, kColorFormat, extent,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
             VK_IMAGE_ASPECT_COLOR_BIT),
      depth_(device, kDepthFormat, extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
             VK_IMAGE_ASPECT_DEPTH_BIT),
      readback_(device.CreateHostBuffer(VkDeviceSize{extent.width} * extent.height * 4U,
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT))
{
    std::array<VkAttachmentDescription, 2> attachments{};
    attachments[0].format = kColorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format = kDepthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    VkSubpassDependency to_transfer{};
    to_transfer.srcSubpass = 0;
    to_transfer.dstSubpass = VK_SUBPASS_EXTERNAL;
    to_transfer.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_transfer.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    to_transfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    pass.pAttachments = attachments.data();
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 1;
    pass.pDependencies = &to_transfer;
    Check(vkCreateRenderPass(device_.Device(), &pass, nullptr, &render_pass_),
          "vkCreateRenderPass");
    std::array<VkImageView, 2> views{color_.View(), depth_.View()};
    VkFramebufferCreateInfo framebuffer{};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = render_pass_;
    framebuffer.attachmentCount = static_cast<std::uint32_t>(views.size());
    framebuffer.pAttachments = views.data();
    framebuffer.width = extent.width;
    framebuffer.height = extent.height;
    framebuffer.layers = 1;
    Check(vkCreateFramebuffer(device_.Device(), &framebuffer, nullptr, &framebuffer_),
          "vkCreateFramebuffer");
}

OffscreenTarget::~OffscreenTarget()
{
    vkDestroyFramebuffer(device_.Device(), framebuffer_, nullptr);
    vkDestroyRenderPass(device_.Device(), render_pass_, nullptr);
}

void OffscreenTarget::Begin(VkCommandBuffer commands) const
{
    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{0.30F, 0.36F, 0.44F, 1.0F}};
    clears[1].depthStencil = {1.0F, 0};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = render_pass_;
    begin.framebuffer = framebuffer_;
    begin.renderArea = {{0, 0}, extent_};
    begin.clearValueCount = static_cast<std::uint32_t>(clears.size());
    begin.pClearValues = clears.data();
    vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
}

void OffscreenTarget::EndAndCopy(VkCommandBuffer commands) const
{
    vkCmdEndRenderPass(commands);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(commands, color_.Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_.Handle(), 1, &copy);
}

std::vector<std::uint8_t> OffscreenTarget::Pixels()
{
    std::span<std::uint8_t> bytes = readback_.Bytes();
    return {bytes.begin(), bytes.end()};
}

} // namespace gears::engine::render
