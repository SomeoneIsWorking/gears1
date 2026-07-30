#include "gpu_shared_device.h"

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

void PublishSharedGpu(const SharedGpu& gpu)
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
    lucent::info("gpu", "shared Vulkan device published: queue family {}, {}",
        gpu.queueFamily,
        gpu.checkedForPresent ? "chosen against a real surface so it can present"
                              : "chosen headless, so presentation is UNVERIFIED");
}

bool AdoptSharedGpu(SharedGpu& out)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_havePublished)
        return false;
    out = g_published;
    return true;
}

bool SharedGpuPublished()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_havePublished;
}

} // namespace gears
