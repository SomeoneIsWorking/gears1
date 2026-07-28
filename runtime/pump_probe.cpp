#include "pump_probe.h"

#include <chrono>
#include <string>

#include <lucent/log.h>

namespace gears
{

thread_local bool t_inAudioPumpCallback = false;

namespace
{

uint64_t NowNanos()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Touched only by the pump thread (record under its thread-local flag, report
// from its loop), so plain fields are correct.
struct Site
{
    const char* name = nullptr;
    uint64_t count = 0;
    uint64_t nanos = 0;
    bool isCount = false;
};
constexpr size_t kMaxSites = 16;
Site g_sites[kMaxSites];

Site* FindSite(const char* name, bool isCount)
{
    for (auto& site : g_sites)
    {
        if (site.name == name)
            return &site;
        if (site.name == nullptr)
        {
            site.name = name;
            site.isCount = isCount;
            return &site;
        }
    }
    return nullptr; // more sites than slots: the extras are dropped, visibly
                    // absent from the report rather than silently merged
}

} // namespace

void PumpWaitRecord(const char* site, uint64_t nanos)
{
    if (Site* s = FindSite(site, false))
    {
        ++s->count;
        s->nanos += nanos;
    }
}

void PumpWaitCount(const char* site, uint64_t n)
{
    if (Site* s = FindSite(site, true))
    {
        ++s->count;
        s->nanos += n;
    }
}

void PumpWaitReport()
{
    std::string line;
    for (auto& site : g_sites)
    {
        if (!site.name || site.count == 0)
            continue;
        line += line.empty() ? "" : ", ";
        line += site.name;
        if (site.isCount)
            line += " " + std::to_string(site.nanos) + "x in " +
                    std::to_string(site.count) + " calls";
        else
            line += " " + std::to_string(site.nanos / 1000000) + " ms/" +
                    std::to_string(site.count);
        site.count = 0;
        site.nanos = 0;
    }
    if (!line.empty())
        lucent::info("audio", "pump blocked in: {}", line);
}

PumpWaitScope::PumpWaitScope(const char* site)
    : site_(t_inAudioPumpCallback ? site : nullptr),
      began_(site_ ? NowNanos() : 0)
{
}

PumpWaitScope::~PumpWaitScope()
{
    if (site_)
        PumpWaitRecord(site_, NowNanos() - began_);
}

} // namespace gears
