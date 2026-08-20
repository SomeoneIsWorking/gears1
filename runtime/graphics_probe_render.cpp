#include "graphics_probe_render.h"

#include "gpu_draw.h"
#include "graphics_probe.h"

namespace gears
{

bool RenderFrameWithGraphicsProbe(const FrameDrawInputs &frame)
{
    const uint64_t request = frame.report ? PendingGraphicsProbeRequest() : 0;
    const bool rendered = RenderFrame(frame);
    if (request != 0)
        PublishGraphicsProbe(request, frame, rendered, GuestFramePixels(), GuestFrameWidth(),
                             GuestFrameHeight());
    return rendered;
}

} // namespace gears
