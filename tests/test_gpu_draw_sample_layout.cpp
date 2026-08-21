#include <cstdio>

#include "gpu_draw_sample_layout.h"

int main()
{
    int failures = 0;
    const auto check = [&](bool actual, const char *name)
    {
        if (actual)
            return;
        std::printf("FAIL %s\n", name);
        ++failures;
    };

    const auto one = gears::draw::DeriveDrawSampleLayout(0, 1280, 1440);
    check(one.imageWidth == 1280 && one.imageHeight == 1440 && one.rasterSamples == 1 &&
              one.viewportScaleX == 1 && one.viewportScaleY == 1,
          "1X retains the canonical sample-grid view");

    const auto two = gears::draw::DeriveDrawSampleLayout(1, 1280, 1440);
    check(two.imageWidth == 1280 && two.imageHeight == 720 && two.rasterSamples == 2 &&
              two.viewportScaleX == 1 && two.viewportScaleY == 1,
          "2X uses a native two-sample attachment in guest-pixel coordinates");

    const auto four = gears::draw::DeriveDrawSampleLayout(2, 1280, 1440);
    check(four.imageWidth == 1280 && four.imageHeight == 1440 && four.rasterSamples == 1 &&
              four.viewportScaleX == 2 && four.viewportScaleY == 2,
          "4X retains the expanded sample-grid representation");

    check(!one.IsNativeMultisample() && two.IsNativeMultisample() && !four.IsNativeMultisample(),
          "only the proven 2X path requests native multisampling");

    if (failures == 0)
        std::puts("GPU draw sample-layout tests passed");
    return failures != 0;
}
