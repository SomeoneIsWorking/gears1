#include "gpu_diagnostics_profile.h"

#include <cassert>

int main()
{
    assert(!gears::IsValidGpuDiagnosticsProfile({}));

    gears::GpuDiagnosticsProfile invalid{
        .graphicsInterrupt =
            {
                .workerPoolGlobal = 1,
                .eventStride = 1,
            },
        .representativeDraw =
            {
                .vertexShaderHash = 1,
                .vertexFetchIndex = 96,
                .vertexStrideDwords = 1,
            },
    };
    assert(!gears::IsValidGpuDiagnosticsProfile(invalid));

    invalid.representativeDraw.vertexFetchIndex = 0;
    assert(gears::IsValidGpuDiagnosticsProfile(invalid));
    assert(gears::GpuGraphicsInterruptEventAddress(invalid.graphicsInterrupt, 0x1000, 3) == 0x1003);

    gears::GpuDiagnosticsProfile missing = invalid;
    missing.graphicsInterrupt.workerPoolGlobal = 0;
    assert(!gears::IsValidGpuDiagnosticsProfile(missing));
    missing = invalid;
    missing.graphicsInterrupt.eventStride = 0;
    assert(!gears::IsValidGpuDiagnosticsProfile(missing));
    missing = invalid;
    missing.representativeDraw.vertexShaderHash = 0;
    assert(!gears::IsValidGpuDiagnosticsProfile(missing));
    missing = invalid;
    missing.representativeDraw.vertexStrideDwords = 0;
    assert(!gears::IsValidGpuDiagnosticsProfile(missing));

    const gears::GpuDiagnosticsProfile &profile = gears::LinkedGpuDiagnosticsProfile();
    assert(gears::IsValidGpuDiagnosticsProfile(profile));
    assert(profile.graphicsInterrupt.workerPoolGlobal == 0x82000868);
    assert(profile.graphicsInterrupt.eventArrayOffset == 0x2BDC);
    assert(profile.graphicsInterrupt.eventStride == 0x38);
    assert(profile.representativeDraw.vertexShaderHash == 0x5363d0746b3ef666ull);
    assert(profile.representativeDraw.vertexFetchIndex == 95);
    assert(profile.representativeDraw.vertexStrideDwords == 12);
    assert(gears::GpuGraphicsInterruptEventAddress(profile.graphicsInterrupt, 0x4015B080, 2) ==
           0x4015DCCC);
}
