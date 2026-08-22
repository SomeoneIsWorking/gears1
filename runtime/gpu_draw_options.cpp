#include "gpu_draw_options.h"

#include <cstdlib>

#include <lucent/config.h>

namespace gears::draw
{

FrameOptions ReadFrameOptions()
{
    FrameOptions out;
    out.applyDepthBias = !lucent::config::flag("DRAW_NODEPTHBIAS");
    out.applyTextureSigns = !lucent::config::flag("DRAW_NO_TEX_SIGNS");

    const std::string &textureBindings = lucent::config::text("DRAW_TEX_BINDS");
    if (!textureBindings.empty())
        out.textureBindingsPsHash = std::strtoull(textureBindings.c_str(), nullptr, 16);
    return out;
}

} // namespace gears::draw
