#include "gpu_draw_targets.h"

#include <cassert>

int main()
{
    gears::draw::FrameSurfaceTargetLookup lookup;
    gears::draw::SurfaceTarget oneSample;
    gears::draw::SurfaceTarget multisample;

    assert(lookup.Find(0x2D0, false) == nullptr);
    assert(lookup.Find(0x2D0, true) == nullptr);

    lookup.Remember(0x2D0, false, &oneSample);
    lookup.Remember(0x2D0, true, &multisample);
    assert(lookup.Find(0x2D0, false) == &oneSample);
    assert(lookup.Find(0x2D0, true) == &multisample);

    // The guest field is twelve bits. Refuse an invalid base rather than alias
    // it onto a valid entry in the fixed-size lookup.
    lookup.Remember(0x1000, false, &oneSample);
    assert(lookup.Find(0x1000, false) == nullptr);
    return 0;
}
