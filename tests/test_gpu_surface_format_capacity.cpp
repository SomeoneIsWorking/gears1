#include <cstdio>
#include <set>

#include "gpu_draw_formats.h"
#include "gpu_surface_format_capacity.h"

int main()
{
    int failures = 0;
    const auto check = [&](bool condition, const char *name)
    {
        if (condition)
            return;
        std::printf("FAIL %s\n", name);
        ++failures;
    };

    std::set<uint32_t> capacity;
    check(gears::draw::AccumulateSurfaceFormats(capacity, {0}),
          "the first LDR frame establishes capacity");
    check(capacity == std::set<uint32_t>{0}, "the LDR format is retained");

    check(gears::draw::AccumulateSurfaceFormats(capacity, {0, 3, 4, 12}),
          "a later mixed-HDR frame grows capacity");
    check(capacity == std::set<uint32_t>({0, 3, 4, 12}),
          "growth retains both boot and gameplay formats");
    bool mixed = false;
    check(gears::draw::HostFormatFor(capacity, mixed) == VK_FORMAT_R16G16B16A16_SFLOAT && mixed,
          "the accumulated gameplay formats select the wide HDR container");

    check(!gears::draw::AccumulateSurfaceFormats(capacity, {0, 3}),
          "a narrower later frame reuses the accumulated capacity");
    check(capacity == std::set<uint32_t>({0, 3, 4, 12}),
          "capacity never shrinks with the current frame");

    if (failures == 0)
        std::puts("GPU surface-format capacity tests passed");
    return failures != 0;
}
