#include "gpu_draw_options.h"

#include <cassert>
#include <cstdlib>

#include <lucent/config.h>

int main()
{
    lucent::config::set_prefix("GEARS_TEST_");
    unsetenv("GEARS_TEST_DRAW_NODEPTHBIAS");
    unsetenv("GEARS_TEST_DRAW_NO_TEX_SIGNS");
    unsetenv("GEARS_TEST_DRAW_TEX_BINDS");
    lucent::config::reset_cache();

    const gears::draw::FrameOptions defaults = gears::draw::ReadFrameOptions();
    assert(defaults.applyDepthBias);
    assert(defaults.applyTextureSigns);
    assert(defaults.textureBindingsPsHash == 0);

    setenv("GEARS_TEST_DRAW_NODEPTHBIAS", "1", 1);
    setenv("GEARS_TEST_DRAW_NO_TEX_SIGNS", "1", 1);
    setenv("GEARS_TEST_DRAW_TEX_BINDS", "501ac5d8692bf7b6", 1);
    lucent::config::reset_cache();

    const gears::draw::FrameOptions controls = gears::draw::ReadFrameOptions();
    assert(!controls.applyDepthBias);
    assert(!controls.applyTextureSigns);
    assert(controls.textureBindingsPsHash == 0x501ac5d8692bf7b6ULL);
    return 0;
}
