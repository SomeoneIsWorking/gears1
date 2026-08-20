#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "scanout_gamma.h"

namespace gears::draw
{

struct Renderer;

// Owns the Vulkan scan-out transform that applies the guest's display LUT to
// the alternating images published to the shared-device presenter.
class GpuScanoutGamma
{
  public:
    bool Initialize(Renderer &renderer, const VkImage images[2]);
    bool Apply(Renderer &renderer, VkCommandBuffer commands, uint32_t imageIndex, uint32_t width,
               uint32_t height, const ScanoutGammaLut &lut);
    void Release(VkDevice device);

  private:
    VkImage images_[2]{};
    VkImageView views_[2]{};
    VkBuffer lutBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory lutMemory_ = VK_NULL_HANDLE;
    void *lutMapped_ = nullptr;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSets_[2]{};
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkShaderModule shader_ = VK_NULL_HANDLE;
};

} // namespace gears::draw
