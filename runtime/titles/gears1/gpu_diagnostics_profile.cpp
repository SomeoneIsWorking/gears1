#include "gpu_diagnostics_profile.h"

namespace gears
{
namespace
{

constexpr GpuDiagnosticsProfile kProfile{
    .graphicsInterrupt =
        {
            .workerPoolGlobal = 0x82000868,
            .eventArrayOffset = 0x2BDC,
            .eventStride = 0x38,
        },
    .representativeDraw =
        {
            .vertexShaderHash = 0x5363d0746b3ef666ull,
            .vertexFetchIndex = 95,
            .vertexStrideDwords = 12,
        },
};

static_assert(IsValidGpuDiagnosticsProfile(kProfile));

} // namespace

const GpuDiagnosticsProfile &LinkedGpuDiagnosticsProfile()
{
    return kProfile;
}

} // namespace gears
