#include "gpu_draw_renderer.h"

#include <lucent/log.h>

#include "gpu_extent_capacity.h"

namespace gears::draw
{

void Renderer::EnsurePersistentCapacity(uint32_t requiredWidth,
                                        uint32_t requiredHeight)
{
    RenderExtentCapacity capacity{requiredWidth, requiredHeight};
    if (persistent && RenderExtentNeedsGrowth(
            persistent->width, persistent->height,
            requiredWidth, requiredHeight))
    {
        capacity = GrowRenderExtentCapacity(
            persistent->width, persistent->height,
            requiredWidth, requiredHeight);
        lucent::info("draw", "persistent renderer capacity growing: {}x{} -> {}x{};"
            " waiting for shared-device work before rebuilding extent-sized resources",
            persistent->width, persistent->height, capacity.width, capacity.height);
        // The window presenter may still be blitting a published stage image
        // after the renderer's own fence has signalled. Both use this device.
        vkDeviceWaitIdle(device);
        ReleasePersistent();
    }
    if (!persistent)
    {
        persistent = new RendererPersistent();
        persistent->width = capacity.width;
        persistent->height = capacity.height;
    }
}

} // namespace gears::draw
