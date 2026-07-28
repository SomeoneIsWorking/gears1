#include "wait_probe.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

std::atomic<uint64_t> g_kernelCalls{0};

// One counter per subsystem that can independently stop.
struct ProgressChannel
{
    const char* name;
    std::atomic<uint64_t> count{0};
    uint64_t last = 0;        // watchdog-only
    uint32_t quietSeconds = 0;
    bool reported = false;
};
ProgressChannel g_progress[] = {{"draw"}, {"audio"}};

ProgressChannel* FindProgress(const char* channel)
{
    for (auto& p : g_progress)
        if (std::string_view(p.name) == channel)
            return &p;
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
        std::thread([] {
            lucent::info("stall", "stall detector armed: reports after {} s with no"
                " progress on a subsystem", kStallSeconds);
            uint64_t lastCalls = 0;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const uint64_t calls = g_kernelCalls.load(std::memory_order_relaxed);

                for (auto& channel : g_progress)
                {
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
                    // Nothing has happened on this channel yet at all: a
                    // subsystem that has not started is not a subsystem that
                    // stopped, and reporting it would cry wolf every boot.
                    if (now == 0 || channel.reported)
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
