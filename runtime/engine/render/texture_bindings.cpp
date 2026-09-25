#include "texture_bindings.h"

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

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 1;
    layout.pBindings = &binding;
    Check(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &layout_),
          "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, capacity};
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

VkDescriptorSet TextureBindings::Bind(const GpuTexture &texture)
{
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(vkAllocateDescriptorSets(device_, &allocate, &set), "vkAllocateDescriptorSets");
    VkDescriptorImageInfo image{sampler_, texture.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return set;
}

} // namespace gears::engine::render
