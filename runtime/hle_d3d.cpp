#include "hle_d3d.h"

namespace gears
{
namespace
{

HleD3dDiagnosticsRouter &Router()
{
    static HleD3dDiagnosticsRouter router;
    return router;
}

} // namespace

bool HleD3dDiagnosticsRouter::Install(HleD3dDiagnosticsCallbacks callbacks) noexcept
{
    if (IsInstalled() || callbacks.dumpCensus == nullptr || callbacks.workerCensus == nullptr)
        return false;
    callbacks_ = callbacks;
    return true;
}

bool HleD3dDiagnosticsRouter::IsInstalled() const noexcept
{
    return callbacks_.dumpCensus != nullptr && callbacks_.workerCensus != nullptr;
}

void HleD3dDiagnosticsRouter::DumpCensus(const char *why) const
{
    if (callbacks_.dumpCensus != nullptr)
        callbacks_.dumpCensus(why);
}

void HleD3dDiagnosticsRouter::WorkerCensus() const
{
    if (callbacks_.workerCensus != nullptr)
        callbacks_.workerCensus();
}

bool InstallHleD3dDiagnostics(HleD3dDiagnosticsCallbacks callbacks) noexcept
{
    return Router().Install(callbacks);
}

void HleDumpCensus(const char *why)
{
    Router().DumpCensus(why);
}

void HleWorkerCensus()
{
    Router().WorkerCensus();
}

} // namespace gears
