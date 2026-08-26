#include "gpu_frame_timing.h"

#include <cassert>
#include <cstdint>

int main()
{
    using gears::draw::GpuTimestampElapsedTicks;

    assert(GpuTimestampElapsedTicks(100, 350, 64) == 250);
    assert(GpuTimestampElapsedTicks(250, 5, 8) == 11);
    assert(GpuTimestampElapsedTicks(0x1234, 0x1244, 12) == 16);
    assert(GpuTimestampElapsedTicks(1, 2, 0) == 0);
    return 0;
}
