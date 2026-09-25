#include "texture_bindings.h"

#include <array>

namespace gears::engine::render
{

TextureBindings::TextureBindings(const VulkanDevice &device, std::uint32_t capacity)
    : device_(device.Device())
{
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    Check(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "vkCreateSampler");

    std::array<VkDescriptorSetLayoutBinding, kBindings> bindings{};
    for (std::uint32_t i = 0; i < kBindings; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = kBindings;
    layout.pBindings = bindings.data();
    Check(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &layout_),
          "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, capacity * kBindings};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = capacity;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    Check(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_), "vkCreateDescriptorPool");
}

TextureBindings::~TextureBindings()
{
    vkDestroyDescriptorPool(device_, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    vkDestroySampler(device_, sampler_, nullptr);
}

VkDescriptorSet TextureBindings::Bind(const GpuTexture &color, const GpuTexture &opacity)
{
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(vkAllocateDescriptorSets(device_, &allocate, &set), "vkAllocateDescriptorSets");
    std::array<VkDescriptorImageInfo, kBindings> images{{
        {sampler_, color.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler_, opacity.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    }};
    std::array<VkWriteDescriptorSet, kBindings> writes{};
    for (std::uint32_t i = 0; i < kBindings; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_, kBindings, writes.data(), 0, nullptr);
    return set;
}

} // namespace gears::engine::render
