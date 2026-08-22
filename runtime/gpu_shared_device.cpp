#include "gpu_shared_device.h"

#include "frame_contract.h"

#include <mutex>

#include <lucent/log.h>

namespace gears
{
namespace
{

std::mutex g_mutex;
SharedGpu g_published;
bool g_havePublished = false;

} // namespace

void PublishSharedGpu(const SharedGpu &gpu)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_havePublished)
    {
        // Not fatal, but it means two devices exist after all and the readback this
        // whole mechanism removes is quietly back. Worth saying out loud.
        lucent::warn("gpu", "a second Vulkan device was published; the first is"
                            " kept, and whichever side created the second is still using its own");
        return;
    }
    g_published = gpu;
    g_havePublished = true;
    lucent::info("gpu", "shared Vulkan device published: queue family {}, {}", gpu.queueFamily,
                 gpu.checkedForPresent ? "chosen against a real surface so it can present"
                                       : "chosen headless, so presentation is UNVERIFIED");
}

bool AdoptSharedGpu(SharedGpu &out)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_havePublished)
        return false;
    out = g_published;
    return true;
}

namespace
{
std::mutex g_frameMutex;
SharedFrameImage g_frame;
FrameContract g_frameContract;
} // namespace

bool PublishSharedFrameImage(const SharedFrameImage &frame)
{
    std::lock_guard<std::mutex> guard(g_frameMutex);
    const FrameTransition transition = g_frameContract.Publish(FrameId{frame.sequence});
    if (transition != FrameTransition::kAdvanced)
    {
        lucent::error("gpu", "shared frame publication rejected sequence {} (transition {})",
                      frame.sequence, static_cast<unsigned>(transition));
        return false;
    }
    g_frame = frame;
    return true;
}

bool AcquireSharedFrameImage(SharedFrameImage &out)
{
    std::lock_guard<std::mutex> guard(g_frameMutex);
    if (g_frame.image == VK_NULL_HANDLE)
        return false;
    out = g_frame;
    const FrameTransition transition = g_frameContract.Present(FrameId{out.sequence});
    if (transition == FrameTransition::kAdvanced || transition == FrameTransition::kRepeated)
        return true;
    lucent::error("gpu", "shared frame acquisition rejected sequence {} (transition {})",
                  out.sequence, static_cast<unsigned>(transition));
    return false;
}

bool SharedGpuPublished()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_havePublished;
}

} // namespace gears
