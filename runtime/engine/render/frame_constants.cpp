#include "frame_constants.h"

#include <cstring>

namespace gears::engine::render
{

FrameConstants::FrameConstants(const VulkanDevice &device)
    : device_(device.Device()),
      buffer_(device.CreateHostBuffer(sizeof(scene::Matrix), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 1;
    layout.pBindings = &binding;
    Check(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &layout_),
          "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    Check(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_), "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layout_;
    Check(vkAllocateDescriptorSets(device_, &allocate, &set_), "vkAllocateDescriptorSets");

    VkDescriptorBufferInfo info{buffer_.Handle(), 0, sizeof(scene::Matrix)};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

FrameConstants::~FrameConstants()
{
    vkDestroyDescriptorPool(device_, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
}

void FrameConstants::Write(const scene::Matrix &view_projection)
{
    std::memcpy(buffer_.Bytes().data(), &view_projection, sizeof(view_projection));
}

} // namespace gears::engine::render
