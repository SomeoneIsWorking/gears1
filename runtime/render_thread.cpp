#include "render_thread.h"

#include "graphics_probe_render.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <lucent/log.h>

#include "render_retirement.h"

namespace gears
{

namespace
{

std::mutex g_mutex;
std::condition_variable g_wake; // renderer waits for work
std::condition_variable g_idle; // WaitForRenderIdle waits for the renderer
struct PendingFrame
{
    FrameDrawInputs inputs;
    uint64_t generation = 0;
};
std::optional<PendingFrame> g_pending;
RenderRetirement g_retirement;
bool g_rendering = false;
bool g_stop = false;
bool g_started = false;
std::thread g_thread;

std::atomic<uint64_t> g_submitted{0};
std::atomic<uint64_t> g_dropped{0};
std::atomic<uint64_t> g_rendered{0};
std::atomic<uint64_t> g_busyMillis{0};
// Wall against CPU, the same discriminator the audio pump uses (catalog #43): a
// renderer that is genuinely slow burns thread CPU time roughly equal to its wall
// time, while one that is descheduled shows wall far above CPU. The live frame
// costs 53 ms against 29 ms for the same frame replayed offline, and those two
// explanations want opposite fixes.
std::atomic<uint64_t> g_cpuMillis{0};
std::atomic<uint64_t> g_runqueueMillis{0};

uint64_t ThreadCpuNanos()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// The scheduler's own account of time spent RUNNABLE but not running.
uint64_t ThreadRunqueueNanos()
{
    std::FILE *f = std::fopen("/proc/thread-self/schedstat", "r");
    if (!f)
        return 0;
    unsigned long long ran = 0, waited = 0, slices = 0;
    const int got = std::fscanf(f, "%llu %llu %llu", &ran, &waited, &slices);
    std::fclose(f);
    return got == 3 ? waited : 0;
}

void RenderThreadMain()
{
    // NICED DOWN, deliberately. This thread is the only one in the process whose
    // work is allowed to be skipped: at most one newer frame waits, and further
    // arrivals are dropped. The guest's threads have no such freedom -- the
    // audio mixer is a hand-off at 187.5 Hz and every slot it misses is heard --
    // so when the machine is short of cores, the renderer is the one that should
    // wait. Measured with the renderer at normal priority:
    // the guest's frame loop recovered to 28 fps but the audio pump fell to
    // 57-67 Hz against 187.5.
    //
    // setpriority on a thread id, which on Linux is what the scheduler actually
    // weights; it needs no privileges to lower a priority.
    const int tid = int(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, id_t(tid), 5) != 0)
        lucent::warn("draw",
                     "render thread: could not nice down (errno {});"
                     " it will compete with the guest's audio for cores",
                     errno);

    for (;;)
    {
        FrameDrawInputs work;
        uint64_t workGeneration = 0;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait(lock, [] { return g_pending.has_value() || g_stop; });
            if (!g_pending.has_value())
            {
                // The wait predicate permits this state only for shutdown.
                // Keeping the check explicit also makes the optional access
                // locally proven rather than dependent on the predicate.
                if (g_stop)
                    return;
                continue;
            }
            work = std::move(g_pending->inputs);
            workGeneration = g_pending->generation;
            g_pending.reset();
            g_rendering = true;
        }

        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t cpu0 = ThreadCpuNanos();
        const uint64_t rq0 = ThreadRunqueueNanos();
        RenderFrameWithGraphicsProbe(work);
        g_busyMillis.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - t0)
                                            .count()));
        g_cpuMillis.fetch_add((ThreadCpuNanos() - cpu0) / 1000000ull);
        g_runqueueMillis.fetch_add((ThreadRunqueueNanos() - rq0) / 1000000ull);
        g_rendered.fetch_add(1);

        // A retirement belongs to the accepted frame that preceded it, not to
        // global renderer idleness. A one-frame pending queue may already hold
        // newer work here; publishing that newer generation now would let the
        // guest recycle its inputs before they have been read.
        for (;;)
        {
            RenderRetirement::FinishBatch batch;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                batch = g_retirement.Finish(workGeneration);
                if (batch.generationComplete)
                {
                    g_rendering = false;
                    break;
                }
            }
            for (RenderRetirement::Completion &completion : batch.completions)
                completion();
        }
        g_idle.notify_all();
    }
}

} // namespace

bool SubmitFrameForRender(FrameDrawInputs &&in)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_started)
    {
        g_started = true;
        g_thread = std::thread(RenderThreadMain);
        lucent::info("draw", "render thread started: the command processor hands over"
                             " each frame's draw list and returns; one newer frame may wait while"
                             " rendering, and further stale arrivals are dropped");
    }
    g_submitted.fetch_add(1);
    // Keep the renderer saturated with at most one waiting frame. With a 53 ms
    // renderer and a 30 Hz guest, dropping every arrival while busy renders at
    // only ~11 fps: each 33 ms arrival misses the completion boundary and the
    // renderer then sits idle until the following one. One pending frame raises
    // that to the renderer's real ~19 fps capacity while bounding latency. A
    // second waiting frame is stale, so that one is counted and dropped.
    if (g_pending.has_value())
    {
        g_dropped.fetch_add(1);
        return false;
    }
    const uint64_t generation = g_retirement.AcceptFrame();
    g_pending = PendingFrame{std::move(in), generation};
    lock.unlock();
    g_wake.notify_one();
    return true;
}

RenderThreadStats RenderThreadCounters()
{
    RenderThreadStats out;
    out.submitted = g_submitted.load();
    out.dropped = g_dropped.load();
    out.rendered = g_rendered.load();
    out.busyMillis = g_busyMillis.load();
    out.cpuMillis = g_cpuMillis.load();
    out.runqueueMillis = g_runqueueMillis.load();
    return out;
}

void DeferUntilAcceptedRenderRetires(std::function<void()> completion)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_retirement.Defer(completion))
            return;
    }
    completion();
}

void WaitForRenderIdle()
{
    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_started)
        return;
    g_idle.wait(lock, [] { return !g_rendering && !g_pending.has_value(); });
}

void StopRenderThread()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_started || g_stop)
            return;
        g_stop = true;
    }
    g_wake.notify_all();
    if (g_thread.joinable())
        g_thread.join();
}

} // namespace gears
