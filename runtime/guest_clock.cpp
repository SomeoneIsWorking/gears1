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
std::atomic<int> g_trigger{int(ClockTrigger::kPresent)};

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

ClockTrigger GuestClockTrigger()
{ return ClockTrigger(g_trigger.load(std::memory_order_relaxed)); }

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
    const std::string& trig = lucent::config::text("GUEST_CLOCK_TRIGGER");
    ClockTrigger trigger = ClockTrigger::kPresent;
    if (trig.empty() || trig == "present")
        trigger = ClockTrigger::kPresent;
    else if (trig == "vblank")
        trigger = ClockTrigger::kVblank;
    else if (trig == "vblank-freerun")
        trigger = ClockTrigger::kVblankFreeRun;
    else
        // REFUSING TO GUESS, for the same reason the step parse does: a typo
        // that silently fell back to `present` would deadlock, and the deadlock
        // would be blamed on the mode rather than on the spelling.
        lucent::error("time", "GEARS_GUEST_CLOCK_TRIGGER='{}' is not one of"
            " present / vblank / vblank-freerun. Falling back to 'present',"
            " which is MEASURED TO DEADLOCK this title when a fixed step is"
            " set -- fix the spelling", trig);
    g_trigger.store(int(trigger), std::memory_order_relaxed);

    if (step)
    {
        switch (trigger)
        {
        case ClockTrigger::kPresent:
            lucent::warn("time", "guest clock trigger is PRESENT (VdSwap). This"
                " is MEASURED TO DEADLOCK this title: it spins in guest code"
                " waiting for time before its first present, so no frame is ever"
                " presented (0 frames against 9 for the real clock). Use"
                " GEARS_GUEST_CLOCK_TRIGGER=vblank-freerun");
            break;
        case ClockTrigger::kVblank:
            lucent::warn("time", "guest clock trigger is VBLANK, host-paced at"
                " 60 Hz. It cannot deadlock, but the host sleep pacing it is"
                " real time entering the guest's clock, so this run is NOT"
                " reproducible. Control arm only -- do not quote a comparison"
                " taken under it");
            break;
        case ClockTrigger::kVblankFreeRun:
            lucent::warn("time", "guest clock trigger is VBLANK-FREERUN, which is"
                " MEASURED TO FREEZE THE PICTURE: the title keeps presenting but"
                " every frame from about 2,700 is bit-identical to the last,"
                " where the real clock's consecutive frames are 21-34%"
                " identical. A frozen picture is trivially reproducible, so the"
                " determinism control looks PERFECT under this mode and means"
                " nothing. NOT USABLE for a comparison");
            break;
        }
    }

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
