#include "guest_clock.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears
{
namespace
{

std::atomic<uint64_t> g_stepNs{0}; // 0 means real time
std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_initialised{false};
std::atomic<int> g_trigger{int(ClockTrigger::kPresent)};
std::atomic<uint32_t> g_vblankSlack{2};

uint64_t HostNanoseconds()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
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
{
    return ClockTrigger(g_trigger.load(std::memory_order_relaxed));
}

uint32_t GuestClockVblankSlack()
{
    return g_vblankSlack.load(std::memory_order_relaxed);
}

bool GuestClockIsFixedStep()
{
    return g_stepNs.load(std::memory_order_relaxed) != 0;
}
uint64_t GuestClockStepNanoseconds()
{
    return g_stepNs.load(std::memory_order_relaxed);
}

void InitGuestClock()
{
    if (g_initialised.exchange(true))
        return;

    uint64_t step = 0;
    const std::string &text = lucent::config::text("GUEST_CLOCK_STEP_NS");
    if (!text.empty())
    {
        char *end = nullptr;
        const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
        if (end == text.c_str() || (end && *end != '\0'))
        {
            // REFUSING TO GUESS. A typo that silently left the real clock
            // running would produce a comparison that looks fixed-step and is
            // not, which is the whole failure this mode exists to avoid.
            lucent::error("time",
                          "GEARS_GUEST_CLOCK_STEP_NS='{}' is not a"
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
    const std::string &trig = lucent::config::text("GUEST_CLOCK_TRIGGER");
    ClockTrigger trigger = ClockTrigger::kPresent;
    if (trig.empty() || trig == "present")
        trigger = ClockTrigger::kPresent;
    else if (trig == "vblank")
        trigger = ClockTrigger::kVblank;
    else if (trig == "vblank-freerun")
        trigger = ClockTrigger::kVblankFreeRun;
    else if (trig == "vblank-paced")
        trigger = ClockTrigger::kVblankPaced;
    else
        // REFUSING TO GUESS, for the same reason the step parse does: a typo
        // that silently fell back to `present` would deadlock, and the deadlock
        // would be blamed on the mode rather than on the spelling.
        lucent::error(
            "time",
            "GEARS_GUEST_CLOCK_TRIGGER='{}' is not one of"
            " present / vblank / vblank-freerun / vblank-paced. Falling back to 'present',"
            " which is MEASURED TO DEADLOCK this title when a fixed step is"
            " set -- fix the spelling",
            trig);
    g_trigger.store(int(trigger), std::memory_order_relaxed);
    // Slack 0 deadlocks by construction -- the guest needs a vblank in flight to
    // produce the present that authorises the next one -- so it is refused
    // rather than accepted into a hang nobody would connect to this setting.
    const long slack = lucent::config::number("GUEST_CLOCK_VBLANK_SLACK", 2);
    if (slack < 1)
        lucent::error("time",
                      "GEARS_GUEST_CLOCK_VBLANK_SLACK={} would deadlock:"
                      " the guest needs a vblank in flight to produce the present that"
                      " authorises the next one. Using 1",
                      slack);
    g_vblankSlack.store(uint32_t(slack < 1 ? 1 : slack), std::memory_order_relaxed);

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
        case ClockTrigger::kVblankPaced:
            lucent::info("time",
                         "guest clock trigger is VBLANK-PACED: vblanks"
                         " free-run until the first present, then are pinned to at most"
                         " {} ahead of the present count. No host clock is read in either"
                         " phase. Reproducibility is NOT thereby guaranteed -- the title's"
                         " other threads are still host threads the OS schedules -- so"
                         " measure it with the determinism control AND the liveness check"
                         " (instrument I027) before quoting any number",
                         g_vblankSlack.load(std::memory_order_relaxed));
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
        lucent::info("time",
                     "guest clock is FIXED STEP: {} ns per presented"
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
