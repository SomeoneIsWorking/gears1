#include "hle_d3d.h"

#include <cassert>
#include <string_view>

namespace
{

int g_dumpCalls = 0;
int g_workerCalls = 0;
int g_otherDumpCalls = 0;
std::string_view g_lastReason;

void Dump(const char *why)
{
    ++g_dumpCalls;
    g_lastReason = why;
}

void Worker()
{
    ++g_workerCalls;
}

void OtherDump(const char *)
{
    ++g_otherDumpCalls;
}

} // namespace

int main()
{
    gears::HleD3dDiagnosticsRouter router;

    assert(!router.IsInstalled());
    router.DumpCensus("unconfigured");
    router.WorkerCensus();
    assert(g_dumpCalls == 0);
    assert(g_workerCalls == 0);

    assert(!router.Install({.dumpCensus = Dump}));
    assert(!router.Install({.workerCensus = Worker}));
    assert(!router.IsInstalled());

    assert(router.Install({.dumpCensus = Dump, .workerCensus = Worker}));
    assert(router.IsInstalled());
    router.DumpCensus("frame 17");
    router.WorkerCensus();
    assert(g_dumpCalls == 1);
    assert(g_workerCalls == 1);
    assert(g_lastReason == "frame 17");

    assert(!router.Install({.dumpCensus = OtherDump, .workerCensus = Worker}));
    router.DumpCensus("still first");
    assert(g_dumpCalls == 2);
    assert(g_otherDumpCalls == 0);
    assert(g_lastReason == "still first");
}
