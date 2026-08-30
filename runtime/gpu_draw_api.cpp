#include "gpu_draw.h"
#include "gpu_draw_native_input.h"

#include <utility>

#include <lucent/log.h>

#ifdef GEARS_HAVE_GUEST_DRAW

#include "gpu_draw_renderer.h"
#include "gpu_queue_access.h"

namespace gears
{
namespace draw
{

std::vector<uint8_t> g_frame;

} // namespace draw
namespace
{

// Rebuilding the renderer per frame made a frame cost roughly 300 ms. This is
// the process-wide owner for the persistent device, targets, pipelines, and
// bounded frame slots.
draw::Renderer &FrameRenderer()
{
    static draw::Renderer renderer;
    static const bool initialized = renderer.Init();
    (void)initialized;
    return renderer;
}

} // namespace

bool RenderFrame(const FrameDrawInputs &in)
{
    draw::Renderer &renderer = FrameRenderer();
    if (renderer.device == VK_NULL_HANDLE)
    {
        draw::g_frame.clear();
        if (in.materializationCallback)
            in.materializationCallback(
                in.sequence >= 0 ? static_cast<uint64_t>(in.sequence) : 0,
                {.status = draw::NativeFrameMaterializationStatus::RendererUnavailable});
        return false;
    }
    const bool rendered = renderer.RenderFrameImpl(in);
    if (!rendered)
        draw::g_frame.clear();
    return rendered;
}

bool SubmitFrameRender(const FrameDrawInputs &in, FrameRenderCompletion &&completion)
{
    if (!completion)
        return false;
    draw::Renderer &renderer = FrameRenderer();
    if (renderer.device == VK_NULL_HANDLE)
    {
        draw::g_frame.clear();
        if (in.materializationCallback)
            in.materializationCallback(
                in.sequence >= 0 ? static_cast<uint64_t>(in.sequence) : 0,
                {.status = draw::NativeFrameMaterializationStatus::RendererUnavailable});
        completion(false);
        return false;
    }
    bool pending = false;
    const bool accepted = renderer.RenderFrameImpl(in, completion, &pending);
    if (!pending)
        completion(accepted);
    if (!accepted)
        draw::g_frame.clear();
    return accepted;
}

bool WaitForRendererGpuIdle()
{
    draw::Renderer &renderer = FrameRenderer();
    return renderer.device == VK_NULL_HANDLE || renderer.frameSlots.WaitInFlight();
}

void ResetRendererForComparison()
{
    draw::Renderer &renderer = FrameRenderer();
    if (renderer.device != VK_NULL_HANDLE)
    {
        SharedGpuQueueAccess().WaitDeviceIdle(renderer.device);
        renderer.ReleasePersistent();
    }
}

void ShutdownRenderer()
{
    FrameRenderer().Shutdown();
}

const std::vector<uint8_t> &GuestFramePixels()
{
    return draw::g_frame;
}

uint32_t GuestFrameWidth()
{
    return draw::kWidth;
}

uint32_t GuestFrameHeight()
{
    return draw::kHeight;
}

} // namespace gears

#else

namespace gears
{

bool RenderFrame(const FrameDrawInputs &in)
{
    lucent::warn("draw", "built without the guest-draw backend"
                         " (needs Vulkan + the Xenos translator)");
    if (in.materializationCallback)
        in.materializationCallback(
            in.sequence >= 0 ? static_cast<uint64_t>(in.sequence) : 0,
            {.status = draw::NativeFrameMaterializationStatus::RendererUnavailable});
    return false;
}

bool SubmitFrameRender(const FrameDrawInputs &in, FrameRenderCompletion &&completion)
{
    if (in.materializationCallback)
        in.materializationCallback(
            in.sequence >= 0 ? static_cast<uint64_t>(in.sequence) : 0,
            {.status = draw::NativeFrameMaterializationStatus::RendererUnavailable});
    if (completion)
        completion(false);
    return false;
}

bool WaitForRendererGpuIdle()
{
    return true;
}

void ShutdownRenderer()
{
}

const std::vector<uint8_t> &GuestFramePixels()
{
    static const std::vector<uint8_t> empty;
    return empty;
}

uint32_t GuestFrameWidth()
{
    return 0;
}

uint32_t GuestFrameHeight()
{
    return 0;
}

} // namespace gears

#endif
