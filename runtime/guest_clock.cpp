#include "guest_clock.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

#include <lucent/config.h>
#include <lucent/log.h>

#include "import_stub.h"

namespace gears
{
namespace
{

std::atomic<uint64_t> g_stepNs{0};       // 0 means real time
std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_initialised{false};
std::atomic<bool> g_onVblank{false};

uint64_t HostNanoseconds()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The host clock's value when the run started, so the guest's clock starts near
// zero in both modes. Without this the fixed-step clock would jump backwards
// relative to the real one the moment it engaged, and mftb MUST NOT go
// backwards -- a title that sees that computes a negative delta.
const uint64_t g_hostStart = HostNanoseconds();

} // namespace

uint64_t GuestClockNanoseconds()
{
    const uint64_t step = g_stepNs.load(std::memory_order_relaxed);
    if (step == 0)
        return HostNanoseconds() - g_hostStart;
    return g_frames.load(std::memory_order_relaxed) * step;
}

void AdvanceGuestClockFrame()
{
    // Incremented unconditionally, even in real-time mode. The counter costs
    // nothing and keeping it live means switching modes cannot expose a path
    // that was never exercised.
    g_frames.fetch_add(1, std::memory_order_relaxed);
}

bool GuestClockOnVblank() { return g_onVblank.load(std::memory_order_relaxed); }

bool GuestClockIsFixedStep() { return g_stepNs.load(std::memory_order_relaxed) != 0; }
uint64_t GuestClockStepNanoseconds() { return g_stepNs.load(std::memory_order_relaxed); }

void InitGuestClock()
{
    if (g_initialised.exchange(true))
        return;

    uint64_t step = 0;
    const std::string& text = lucent::config::text("GUEST_CLOCK_STEP_NS");
    if (!text.empty())
    {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
        if (end == text.c_str() || (end && *end != '\0'))
        {
            // REFUSING TO GUESS. A typo that silently left the real clock
            // running would produce a comparison that looks fixed-step and is
            // not, which is the whole failure this mode exists to avoid.
            lucent::error("time", "GEARS_GUEST_CLOCK_STEP_NS='{}' is not a"
                " number of nanoseconds. The guest clock is left on REAL TIME"
                " and this run is NOT reproducible; fix the value or unset it",
                text);
        }
        else
        {
            step = uint64_t(v);
        }
    }
    g_stepNs.store(step, std::memory_order_relaxed);
    g_onVblank.store(lucent::config::flag("GUEST_CLOCK_ON_VBLANK"),
                     std::memory_order_relaxed);
    if (g_onVblank.load(std::memory_order_relaxed))
        lucent::warn("time", "GEARS_GUEST_CLOCK_ON_VBLANK: the clock is stepped"
            " by the 60 Hz VBLANK thread, not by VdSwap. That thread is paced by"
            " a HOST SLEEP, so this run is NOT reproducible. It is a control arm"
            " for telling a clock-frozen stall from any other stall -- do not"
            " quote a comparison taken under it");

    // ALWAYS reported, in both modes.
    if (step)
        lucent::info("time", "guest clock is FIXED STEP: {} ns per presented"
            " frame ({:.3f} Hz). Every clock the guest reads -- mftb, the"
            " KeTimeStampBundle and KeQuerySystemTime -- advances only at"
            " VdSwap, so the simulation is a function of the input schedule"
            " alone. Audio and any timebase-bounded wait see this clock too",
            step, 1e9 / double(step));
    else
        lucent::info("time", "guest clock is REAL TIME (GEARS_GUEST_CLOCK_STEP_NS"
            " unset). Two runs of the same input script will NOT reach the same"
            " game state at the same frame -- catalog #84 measures 17.7%"
            " identical pixels by frame 1200. Set it to compare runs");
}

} // namespace gears

// Installed into the recompiled code's timebase reader. Generated code calls
// __ppc_time_base() directly for every mftb, so the hook has to live in
// XenonRecomp's header; this is the runtime half that fills it in.
namespace
{
uint64_t GuestTimeBaseTicks()
{
    // Widened so the multiply cannot overflow before the divide.
    return uint64_t((__uint128_t(gears::GuestClockNanoseconds()) *
                     PPC_TIME_BASE_FREQUENCY) / 1000000000ull);
}

const bool g_timeBaseInstalled = [] {
    __ppc_set_time_base_source(&GuestTimeBaseTicks);
    return true;
}();
} // namespace
