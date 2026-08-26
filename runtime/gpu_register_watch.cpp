#include "gpu_register_watch.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears
{
namespace
{

std::map<uint32_t, uint64_t> g_hits;
std::string g_source = "?";

} // namespace

const std::vector<uint32_t> &GpuRegisterWatchRegisters()
{
    static const std::vector<uint32_t> watch = []
    {
        std::vector<uint32_t> result;
        const std::string &text = lucent::config::text("GPU_REG_WATCH");
        size_t at = 0;
        while (at < text.size())
        {
            size_t comma = text.find(',', at);
            if (comma == std::string::npos)
                comma = text.size();
            const std::string item = text.substr(at, comma - at);
            if (!item.empty())
                result.push_back(uint32_t(std::strtoul(item.c_str(), nullptr, 16)));
            at = comma + 1;
        }
        if (!result.empty())
            lucent::info("gpu", "watching {} GPU register(s) for writes", result.size());
        return result;
    }();
    return watch;
}

bool GpuRegisterWatchEnabled()
{
    return !GpuRegisterWatchRegisters().empty();
}

GpuRegisterWriteScope::GpuRegisterWriteScope(std::string source)
    : active_(GpuRegisterWatchEnabled())
{
    if (active_)
    {
        previous_ = std::move(g_source);
        g_source = std::move(source);
    }
}

GpuRegisterWriteScope::~GpuRegisterWriteScope()
{
    if (active_)
        g_source = std::move(previous_);
}

void ObserveGpuRegisterWrite(uint32_t reg, uint32_t value, uint32_t oldBits, uint32_t drawOrdinal)
{
    const std::vector<uint32_t> &watch = GpuRegisterWatchRegisters();
    if (watch.empty() || std::find(watch.begin(), watch.end(), reg) == watch.end())
        return;

    float newFloat;
    float oldFloat;
    std::memcpy(&newFloat, &value, sizeof(newFloat));
    std::memcpy(&oldFloat, &oldBits, sizeof(oldFloat));
    ++g_hits[reg];
    lucent::info("gpu",
                 "reg {:#x} <- {:#010x} ({}), was {:#010x} ({}), from {},"
                 " before draw {} of this frame",
                 reg, value, newFloat, oldBits, oldFloat, g_source, drawOrdinal);
}

void ReportGpuRegisterWatch()
{
    const std::vector<uint32_t> &watch = GpuRegisterWatchRegisters();
    if (watch.empty())
        return;
    static uint64_t frames = 0;
    if (frames++ % 60 != 0)
        return;

    lucent::Line line;
    line.add("GPU_REG_WATCH census (cumulative):");
    for (uint32_t reg : watch)
    {
        const auto it = g_hits.find(reg);
        line.add(" {:#x}={}{}", reg, it == g_hits.end() ? 0 : it->second,
                 it == g_hits.end() ? " (NEVER WRITTEN)" : "");
    }
    line.flush(lucent::Level::Info, "gpu");
}

} // namespace gears
