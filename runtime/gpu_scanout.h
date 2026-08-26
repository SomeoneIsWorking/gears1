#pragma once

#include <cstdint>
#include <memory>

#include <vulkan/vulkan.h>

#include "gpu_scanout_gamma.h"
#include "gpu_shared_device.h"

namespace gears::draw
{

struct Renderer;

struct GpuScanoutResult
{
    VkImage image = VK_NULL_HANDLE;
    SharedFrameImage::Lease lease;
    bool gammaApplied = false;
};

// Owns the leased RGBA8 images that decouple rendering from presentation,
// including the guest display-LUT transform that belongs at scan-out.
class GpuScanout
{
  public:
    bool Initialize(Renderer &renderer, uint32_t width, uint32_t height);
    bool Record(Renderer &renderer, VkCommandBuffer commands, VkImage source, uint32_t width,
                uint32_t height, const uint32_t *guestGammaRamp, VkBuffer readback, bool copyToHost,
                GpuScanoutResult &result);
    bool Publish(const GpuScanoutResult &result, uint32_t width, uint32_t height, long frameId);
    void Release(Renderer &renderer);

  private:
    struct ImageLifetime;
    static constexpr uint32_t kImageCount = GpuScanoutGamma::kImageCount;
    std::shared_ptr<ImageLifetime> images_[kImageCount];
    uint32_t nextImage_ = 0;
    GpuScanoutGamma gamma_;
};

} // namespace gears::draw
