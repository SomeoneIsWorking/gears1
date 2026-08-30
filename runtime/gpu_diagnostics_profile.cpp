#include "gpu_diagnostics_profile.h"

#include <lucent/log.h>

#include "guest_memory.h"

namespace gears
{
namespace
{

const GpuDiagnosticsProfile *ActiveProfile()
{
    static const GpuDiagnosticsProfile *profile = []
    {
        const GpuDiagnosticsProfile &linked = LinkedGpuDiagnosticsProfile();
        if (!IsValidGpuDiagnosticsProfile(linked))
        {
            lucent::error("gpu", "linked title supplied an invalid GPU diagnostics profile");
            return static_cast<const GpuDiagnosticsProfile *>(nullptr);
        }
        return &linked;
    }();
    return profile;
}

std::uint32_t ReadGuest32(std::uint32_t address)
{
    return __builtin_bswap32(*Memory().Translate<std::uint32_t>(address));
}

} // namespace

const GpuRepresentativeDrawDiagnostics *CurrentGpuRepresentativeDrawDiagnostics()
{
    const GpuDiagnosticsProfile *profile = ActiveProfile();
    return profile == nullptr ? nullptr : &profile->representativeDraw;
}

bool ResolveGpuGraphicsInterruptDiagnostics(std::uint32_t cpu,
                                            GpuGraphicsInterruptDiagnosticState &state)
{
    const GpuDiagnosticsProfile *profile = ActiveProfile();
    if (profile == nullptr)
        return false;

    const GpuGraphicsInterruptDiagnostics &layout = profile->graphicsInterrupt;
    state.workerPool = ReadGuest32(ReadGuest32(layout.workerPoolGlobal));
    state.event = GpuGraphicsInterruptEventAddress(layout, state.workerPool, cpu);
    return true;
}

} // namespace gears
