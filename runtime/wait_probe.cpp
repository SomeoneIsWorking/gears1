#include "wait_probe.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <lucent/config.h>
#include <lucent/log.h>

#include "guest_thread.h"

namespace gears
{

thread_local bool t_inAudioPumpCallback = false;

namespace
{
// How long the whole guest may be silent before it counts as stalled. Long
// enough that a slow load or a heavy frame is not reported, short enough to
// catch a hang while the run is still going.
constexpr uint32_t kStallSeconds = 8;

// A subsystem that has NEVER ticked is watched too, on a longer fuse -- see the
// never-started branch in the watchdog loop for why that is a separate state.
constexpr uint32_t kNeverStartedSeconds = 40;

std::atomic<uint64_t> g_kernelCalls{0};

// One counter per subsystem that can independently stop.
struct ProgressChannel
{
    const char* name;
    std::atomic<uint64_t> count{0};
    uint64_t last = 0;        // watchdog-only
    uint32_t quietSeconds = 0;
    bool reported = false;
    // Seconds since the detector armed, counted only while the channel has
    // never ticked at all.
    uint32_t deadSeconds = 0;
    bool neverStartedReported = false;
};
// "selftest.*" are only fed when GEARS_STALL_SELFTEST is set; see
// StartStallDetector.
ProgressChannel g_progress[] = {{"draw"}, {"audio"},
                                {"selftest.stopped"}, {"selftest.silent"}};

// A channel name that matches nothing used to no-op silently, which disables a
// watchdog outright and leaves the log looking exactly like a healthy run. The
// name comes from a string literal at a call site, so a mismatch is a typo that
// nothing else would ever surface.
std::atomic<bool> g_unknownChannelReported{false};

ProgressChannel* FindProgress(const char* channel)
{
    for (auto& p : g_progress)
        if (std::string_view(p.name) == channel)
            return &p;
    if (!g_unknownChannelReported.exchange(true))
        lucent::error("stall", "NoteGuestProgress(\"{}\") names no known channel:"
            " that subsystem is not being watched at all, and its silence in this"
            " log means nothing", channel);
    return nullptr;
}

// One record per guest thread, never removed: a stalled thread's state is
// exactly what the report needs, so entries outlive the threads themselves.
struct ThreadState
{
    std::string name;
    std::atomic<const char*> site{nullptr};
    std::atomic<uint64_t> since{0};
    std::atomic<bool> live{true};
};

std::mutex g_threadsMutex;
std::vector<std::unique_ptr<ThreadState>> g_threads;

ThreadState* MyThreadState()
{
    thread_local ThreadState* mine = [] {
        // Host threads that never run guest code are not interesting here, and
        // registering them would make the report longer without making it
        // clearer.
        const char* name = GuestThreadName();
        if (!name || std::string_view(name) == "host")
            return static_cast<ThreadState*>(nullptr);
        auto state = std::make_unique<ThreadState>();
        state->name = name;
        ThreadState* raw = state.get();
        std::lock_guard<std::mutex> guard(g_threadsMutex);
        g_threads.push_back(std::move(state));
        return raw;
    }();
    return mine;
}
} // namespace

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
// Slots are keyed by POINTER, so two identical literals in different
// translation units can occupy two slots -- the table fills faster than the
// count of distinct site names suggests.
uint64_t g_sitesDropped = 0;

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
    // The table is full and this site's time is being thrown away. The comment
    // that used to sit here claimed the extras were "visibly absent from the
    // report" -- they were not: the report printed the sixteen that fitted and
    // said nothing at all about the rest, so the pump blocking hard in a
    // seventeenth site read as the pump not blocking there. Counted, and named
    // in the report, so the omission is visible where the result is.
    ++g_sitesDropped;
    return nullptr;
}

} // namespace

void WaitProbeRecord(const char* site, uint64_t nanos)
{
    if (Site* s = FindSite(site, false))
    {
        ++s->count;
        s->nanos += nanos;
    }
}

void WaitProbeCount(const char* site, uint64_t n)
{
    if (Site* s = FindSite(site, true))
    {
        ++s->count;
        s->nanos += n;
    }
}

void WaitProbeReport()
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
    if (g_sitesDropped)
    {
        line += line.empty() ? "" : ", ";
        line += "AND " + std::to_string(g_sitesDropped) +
                " record(s) from sites that did not fit in the " +
                std::to_string(kMaxSites) +
                "-slot table -- their time is NOT in this line";
        g_sitesDropped = 0;
    }
    if (!line.empty())
        lucent::info("audio", "pump blocked in: {}", line);
}

WaitProbe::WaitProbe(const char* site)
    : site_(t_inAudioPumpCallback ? site : nullptr),
      began_(site_ ? NowNanos() : 0),
      published_(false)
{
    // The heartbeat: any guest thread entering any kernel primitive. Relaxed
    // because it is a liveness signal, not a synchronisation one -- the
    // watchdog only needs to see it change eventually.
    g_kernelCalls.fetch_add(1, std::memory_order_relaxed);

    ThreadState* state = MyThreadState();
    if (state && state->site.load(std::memory_order_relaxed) == nullptr)
    {
        // Only the outermost probe publishes. A wait taken inside a lock would
        // otherwise overwrite the more informative outer site.
        state->since.store(NowNanos(), std::memory_order_relaxed);
        state->site.store(site, std::memory_order_release);
        published_ = true;
    }
}

WaitProbe::~WaitProbe()
{
    if (site_)
        WaitProbeRecord(site_, NowNanos() - began_);
    if (published_)
    {
        if (ThreadState* state = MyThreadState())
            state->site.store(nullptr, std::memory_order_release);
    }
}

void NoteGuestProgress(const char* channel)
{
    if (ProgressChannel* p = FindProgress(channel))
        p->count.fetch_add(1, std::memory_order_relaxed);
}

// PROGRESS IS SUBSYSTEM WORK, NOT KERNEL CALLS. The first version of this
// watched kernel calls and never fired: threads that poll -- a 30 ms timed
// wait, a 500 us poll loop -- keep entering the kernel forever, so a completely
// stuck guest still ticks. Kernel calls are still counted, but as a
// DESCRIPTION of the stall (dead, or spinning?) rather than as the test for it.
// How many kernel calls the guest has made, for the reproducibility question in
// catalog #84: if a deterministic clock is to be derived from GUEST WORK rather
// than from presents or the host clock, the first thing to establish is whether
// a measure of guest work is itself reproducible across runs.
uint64_t GuestKernelCalls() { return g_kernelCalls.load(std::memory_order_relaxed); }

void ReportStall(const char* channel, uint32_t seconds, uint64_t kernelCalls)
{
    lucent::warn("stall", "{} has made no progress for {} s. The guest made {}"
        " kernel calls in the last second ({}), and this is what every guest"
        " thread was doing:", channel, seconds, kernelCalls,
        kernelCalls == 0 ? "nothing is running at all"
                         : "so something is running, just not this");

    const uint64_t now = NowNanos();
    uint32_t running = 0, blocked = 0;
    std::lock_guard<std::mutex> guard(g_threadsMutex);
    for (const auto& state : g_threads)
    {
        if (!state->live.load(std::memory_order_relaxed))
            continue;
        const char* site = state->site.load(std::memory_order_acquire);
        if (site == nullptr)
        {
            ++running;
            lucent::warn("stall", "  {} is running guest code, not in any kernel call",
                         state->name);
        }
        else
        {
            ++blocked;
            const uint64_t since = state->since.load(std::memory_order_relaxed);
            lucent::warn("stall", "  {} has been in {} for {} ms", state->name, site,
                         since && now > since ? (now - since) / 1000000 : 0);
        }
    }
    lucent::warn("stall", "{} thread(s) blocked, {} running guest code. {}",
        blocked, running,
        running ? "A thread running while nothing progresses is a spin -- it is"
                  " waiting on something it polls, not on us."
                : "Every thread is blocked: nothing will signal them and this"
                  " will not recover.");
}

void StartStallDetector()
{
    static std::once_flag started;
    std::call_once(started, [] {
        // SELF-VALIDATION, in the shape of GEARS_ASAN_SELFTEST in main.cpp. A
        // watchdog's normal output is nothing, so a broken one and a healthy
        // guest are indistinguishable from the log. GEARS_STALL_SELFTEST=1
        // feeds two synthetic channels a case each detector branch MUST report:
        //   selftest.stopped -- ticks exactly once, then never again, so the
        //                       stalled-after-starting branch must fire at
        //                       kStallSeconds;
        //   selftest.silent  -- never ticks at all, so the never-started branch
        //                       must fire at kNeverStartedSeconds.
        // A run with the flag set that prints neither report has a detector
        // that cannot detect, and any "no stall was reported" conclusion drawn
        // from a normal run is worthless.
        const bool selftest = lucent::config::flag("STALL_SELFTEST");
        if (selftest)
        {
            lucent::info("stall", "self-test: two synthetic channels armed."
                " 'selftest.stopped' MUST be reported stalled after {} s and"
                " 'selftest.silent' MUST be reported never-started after {} s."
                " If neither line appears, the stall detector is not working"
                " and its silence on the real channels means nothing.",
                kStallSeconds, kNeverStartedSeconds);
            NoteGuestProgress("selftest.stopped");
        }

        std::thread([selftest] {
            lucent::info("stall", "stall detector armed: reports after {} s with no"
                " progress on a subsystem, and after {} s on one that never"
                " started at all", kStallSeconds, kNeverStartedSeconds);
            uint64_t lastCalls = 0;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const uint64_t calls = g_kernelCalls.load(std::memory_order_relaxed);

                for (auto& channel : g_progress)
                {
                    // The synthetic channels are inert unless asked for; they
                    // would otherwise report a never-started subsystem on every
                    // single run, which is the cry-wolf this design avoids.
                    if (!selftest &&
                        std::string_view(channel.name).substr(0, 9) == "selftest.")
                        continue;
                    const uint64_t now = channel.count.load(std::memory_order_relaxed);
                    if (now != channel.last)
                    {
                        if (channel.reported)
                            lucent::info("stall", "{} is progressing again after {} s",
                                         channel.name, channel.quietSeconds);
                        channel.last = now;
                        channel.quietSeconds = 0;
                        channel.reported = false;
                        continue;
                    }
                    // NEVER-STARTED IS ITS OWN STATE, NOT SILENCE. A subsystem
                    // that has not started is genuinely not one that stopped,
                    // and reporting it at kStallSeconds would cry wolf on every
                    // boot -- which is why this used to `continue` outright.
                    // But that made the detector structurally unable to report
                    // the most common hang there is: the guest wedging BEFORE
                    // its first swap or its first mixed audio frame. The
                    // detector armed, printed "armed", and then could only ever
                    // stay quiet, so its silence proved nothing. It is watched
                    // on a longer fuse instead, reported once, and worded so it
                    // cannot be confused with a stall after a healthy start.
                    if (now == 0)
                    {
                        if (channel.neverStartedReported)
                            continue;
                        if (++channel.deadSeconds < kNeverStartedSeconds)
                            continue;
                        channel.neverStartedReported = true;
                        lucent::warn("stall", "{} has NEVER made progress in the"
                            " {} s since the detector armed -- not a stall, a"
                            " subsystem that never started:", channel.name,
                            channel.deadSeconds);
                        ReportStall(channel.name, channel.deadSeconds,
                                    calls - lastCalls);
                        continue;
                    }
                    if (channel.reported)
                        continue;
                    if (++channel.quietSeconds < kStallSeconds)
                        continue;

                    channel.reported = true;
                    ReportStall(channel.name, channel.quietSeconds, calls - lastCalls);
                }
                lastCalls = calls;
            }
        }).detach();
    });
}

} // namespace gears
