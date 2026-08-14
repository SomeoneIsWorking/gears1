#include <cstdio>

#include "gpu_extent_capacity.h"

int main()
{
    int failures = 0;
    const auto check = [&](bool actual, bool expected, const char* name) {
        if (actual == expected)
            return;
        std::printf("FAIL %s: got %d, expected %d\n", name, actual, expected);
        ++failures;
    };

    check(gears::draw::RenderExtentNeedsGrowth(1280, 1440, 1280, 720), false,
          "a one-sample frame reuses a two-sample allocation");
    check(gears::draw::RenderExtentNeedsGrowth(1280, 720, 1280, 1440), true,
          "a two-sample frame grows a one-sample allocation");
    check(gears::draw::RenderExtentNeedsGrowth(1280, 1440, 1280, 1440), false,
          "an equal extent reuses its allocation");
    check(gears::draw::RenderExtentNeedsGrowth(1280, 1440, 1344, 720), true,
          "growth on either axis rebuilds the allocation");

    const auto grown = gears::draw::GrowRenderExtentCapacity(
        1280, 1440, 1344, 720);
    check(grown.width == 1344, true, "a growing width is retained");
    check(grown.height == 1440, true, "growth on one axis does not shrink the other");

    if (failures == 0)
        std::puts("GPU extent capacity tests passed");
    return failures != 0;
}
