#include "gpu_scanout_gamma.h"

#include <cstring>

#include <lucent/log.h>

#include "gpu_draw_renderer.h"
#include "scanout_gamma_spv.h"

namespace gears::draw
{
namespace
{

struct ScanoutGammaPushConstants
{
    uint32_t width;
    uint32_t height;
};

} // namespace

bool GpuScanoutGamma::Initialize(Renderer &renderer, const VkImage images[kImageCount])
{
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(renderer.physical, VK_FORMAT_R8G8B8A8_UNORM,
                                        &formatProperties);
    if ((formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0)
    {
        lucent::error("draw", "scan-out gamma unavailable: the shared Vulkan"
                              " device cannot use R8G8B8A8_UNORM as a storage image");
        return false;
    }

    auto fail = [&]()
    {
        Release(renderer.device);
        return false;
    };

    for (uint32_t i = 0; i < kImageCount; ++i)
    {
        images_[i] = images[i];
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(renderer.device, &viewInfo, nullptr, &views_[i]) != VK_SUCCESS)
            return fail();
        if (!renderer.MakeBuffer(sizeof(ScanoutGammaLut), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 lutBuffers_[i], lutMemory_[i]) ||
            vkMapMemory(renderer.device, lutMemory_[i], 0, sizeof(ScanoutGammaLut), 0,
                        &lutMapped_[i]) != VK_SUCCESS)
            return fail();
    }

    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(renderer.device, &setInfo, nullptr, &setLayout_) != VK_SUCCESS)
        return fail();

    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                        sizeof(ScanoutGammaPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(renderer.device, &layoutInfo, nullptr, &pipelineLayout_) !=
        VK_SUCCESS)
        return fail();

    const std::vector<uint32_t> &code = native::ScanoutGammaSpirv();
    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = code.size() * sizeof(uint32_t);
    shaderInfo.pCode = code.data();
    if (vkCreateShaderModule(renderer.device, &shaderInfo, nullptr, &shader_) != VK_SUCCESS)
        return fail();

    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader_;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout_;
    if (vkCreateComputePipelines(renderer.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                 &pipeline_) != VK_SUCCESS)
        return fail();

    const VkDescriptorPoolSize poolSizes[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kImageCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kImageCount},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = kImageCount;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(renderer.device, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
        return fail();

    VkDescriptorSetLayout layouts[kImageCount]{};
    for (VkDescriptorSetLayout &layout : layouts)
        layout = setLayout_;
    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = kImageCount;
    allocateInfo.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(renderer.device, &allocateInfo, descriptorSets_) != VK_SUCCESS)
        return fail();

    for (uint32_t i = 0; i < kImageCount; ++i)
    {
        const VkDescriptorBufferInfo bufferInfo{lutBuffers_[i], 0, sizeof(ScanoutGammaLut)};
        const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, views_[i], VK_IMAGE_LAYOUT_GENERAL};
        const VkWriteDescriptorSet writes[2] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSets_[i], 0, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &imageInfo, nullptr, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSets_[i], 1, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bufferInfo, nullptr},
        };
        vkUpdateDescriptorSets(renderer.device, 2, writes, 0, nullptr);
    }

    lucent::info("draw", "shared-device scan-out gamma pipeline built");
    return true;
}

bool GpuScanoutGamma::Apply(Renderer &, VkCommandBuffer commands, uint32_t imageIndex,
                            uint32_t width, uint32_t height, const ScanoutGammaLut &lut)
{
    if (imageIndex >= kImageCount || pipeline_ == VK_NULL_HANDLE || !lutMapped_[imageIndex])
        return false;
    std::memcpy(lutMapped_[imageIndex], lut.data(), sizeof(lut));

    VkBufferMemoryBarrier lutBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    lutBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    lutBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    lutBarrier.srcQueueFamilyIndex = lutBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    lutBarrier.buffer = lutBuffers_[imageIndex];
    lutBarrier.size = sizeof(ScanoutGammaLut);
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &lutBarrier, 0, nullptr);

    VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageBarrier.srcQueueFamilyIndex = imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = images_[imageIndex];
    imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &imageBarrier);

    const ScanoutGammaPushConstants push{width, height};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1,
                            &descriptorSets_[imageIndex], 0, nullptr);
    vkCmdPushConstants(commands, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                       &push);
    vkCmdDispatch(commands, (width + 7) / 8, (height + 7) / 8, 1);

    imageBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &imageBarrier);
    return true;
}

void GpuScanoutGamma::Release(VkDevice device)
{
    vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    vkDestroyPipeline(device, pipeline_, nullptr);
    vkDestroyShaderModule(device, shader_, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    for (uint32_t i = 0; i < kImageCount; ++i)
    {
        if (lutMapped_[i])
            vkUnmapMemory(device, lutMemory_[i]);
        vkDestroyBuffer(device, lutBuffers_[i], nullptr);
        vkFreeMemory(device, lutMemory_[i], nullptr);
        vkDestroyImageView(device, views_[i], nullptr);
        lutMapped_[i] = nullptr;
        lutBuffers_[i] = VK_NULL_HANDLE;
        lutMemory_[i] = VK_NULL_HANDLE;
        views_[i] = VK_NULL_HANDLE;
    }
    descriptorPool_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    shader_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

} // namespace gears::draw
