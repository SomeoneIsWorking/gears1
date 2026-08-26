// One Vulkan device, shared between the draw path and the present path.
//
// They each created their own VkInstance and VkDevice, which costs a full readback
// of every rendered frame to host memory and a staging upload back to the GPU,
// purely because the drawn image lives on one device and the swapchain on the
// other. Nothing else requires that; it is an artefact of the two files having been
// brought up independently.
//
// PUBLISH AND ADOPT rather than a factory, deliberately. Whichever side comes up
// first creates the objects it needs and publishes them; the other adopts them
// instead of creating its own. That keeps the diff in each file small and leaves
// both able to stand alone -- the draw path still works headless, where no
// presenter ever runs and there is nothing to adopt.
//
// The ordering is not left to chance: PresenterStart() runs before the command
// processor executes a single packet and blocks until the presenter thread has
// created (and published) its device, while the draw path initialises at the
// first swap that carries draws. So in a windowed run the presenter publishes and
// the draw path adopts. The reverse order is still handled -- see AdoptSharedGpu
// -- because a headless run has no presenter at all and must not depend on one.
#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include <vulkan/vulkan.h>

namespace gears
{

struct SharedGpu
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;

    // Whether the queue family was chosen against a real surface. A device created
    // headless has NOT been checked for presentation support, and a presenter that
    // adopts one must verify it rather than assume -- silently presenting on a
    // family that does not support it is undefined behaviour that shows up as a
    // blank window rather than an error.
    bool checkedForPresent = false;
};

// Publishes the objects for the other side to adopt. Called once, by whoever
// created them. Publishing twice is a programming error and is reported.
void PublishSharedGpu(const SharedGpu &gpu);

// Returns true and fills `out` when another side has already published. False means
// nothing has, and the caller should create its own objects and publish them.
bool AdoptSharedGpu(SharedGpu &out);

// The last rendered frame AS AN IMAGE on the shared device, so the presenter can
// blit it into the swapchain instead of receiving it through host memory. Only
// published when the draw path ADOPTED the presenter's device -- if it created its
// own the image lives on a different device and is useless here, which is the
// headless case.
//
// The image is left in TRANSFER_SRC_OPTIMAL by the draw path and published only
// from its ordered producer-fence completion. `sequence` is the executing swap
// packet's original identity carried unchanged through the renderer. The shared-
// frame contract rejects zero, duplicate, and regressive publications rather
// than allowing scan-out to invent another clock.
struct SharedFrameImage
{
    VkImage image = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t sequence = 0;

    // Retains the scan-out allocation independently of the renderer cache.
    // Copies are intentional: the published latest frame and every presenter
    // submission each keep one until their own lifetime boundary retires.
    class Lease
    {
      public:
        Lease() = default;
        explicit Lease(std::shared_ptr<void> owner) : owner_(std::move(owner)) {}

        [[nodiscard]] bool Valid() const { return owner_ != nullptr; }
        void Reset() { owner_.reset(); }

      private:
        std::shared_ptr<void> owner_;
    } lease;
};

bool PublishSharedFrameImage(const SharedFrameImage &frame);
bool AcquireSharedFrameImage(SharedFrameImage &out);
void ClearSharedFrameImage();

// Whether anything has been published yet, without taking a copy. Used by the
// teardown paths, which must destroy the device exactly once -- whoever adopted it
// does not own it.
bool SharedGpuPublished();

} // namespace gears
