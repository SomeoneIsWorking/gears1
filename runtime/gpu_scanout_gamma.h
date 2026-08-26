#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "gpu_frame_capacity.h"
#include "scanout_gamma.h"

namespace gears::draw
{

struct Renderer;

// Owns the Vulkan scan-out transform that applies the guest's display LUT to
// the alternating images published to the shared-device presenter.
class GpuScanoutGamma
{
  public:
    static constexpr uint32_t kImageCount = kSharedScanoutImageCount;

    bool Initialize(Renderer &renderer, const VkImage images[kImageCount]);
    bool Apply(Renderer &renderer, VkCommandBuffer commands, uint32_t imageIndex, uint32_t width,
               uint32_t height, const ScanoutGammaLut &lut);
    void Release(VkDevice device);

  private:
    VkImage images_[kImageCount]{};
    VkImageView views_[kImageCount]{};
    VkBuffer lutBuffers_[kImageCount]{};
    VkDeviceMemory lutMemory_[kImageCount]{};
    void *lutMapped_[kImageCount]{};
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSets_[kImageCount]{};
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkShaderModule shader_ = VK_NULL_HANDLE;
};

} // namespace gears::draw
