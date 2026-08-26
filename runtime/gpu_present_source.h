#pragma once

#include <cstddef>

#include <vulkan/vulkan.h>

#include "gpu_frame_capacity.h"
#include "gpu_shared_device.h"
#include "gpu_present_stage.h"

namespace gears
{

// Retains each shared scan-out allocation through the presenter submission
// that reads it. A slot becomes reusable only after its fence signals.
class PresentSourceSlots
{
  public:
    static constexpr size_t kCapacity = kPresenterFramesInFlight;

    bool Begin(VkDevice device, VkFence fence, size_t slot);
    struct RecordedFrame
    {
        SharedFrameImage::Lease lease;
        uint64_t sequence = 0;
        bool recorded = false;
    };
    RecordedFrame RecordLatest(VkCommandBuffer commands, VkImage destination, VkExtent2D extent,
                               bool swapchainIsSrgb, SrgbRawCopyStage &srgbStage);
    void Submitted(size_t slot, SharedFrameImage::Lease lease);
    void Release(size_t slot);
    void ResetAfterDeviceIdle();

  private:
    SharedFrameImage::Lease leases_[kCapacity];
    bool submitted_[kCapacity]{};
};

} // namespace gears
