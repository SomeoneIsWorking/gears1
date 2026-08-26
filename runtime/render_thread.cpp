#include "render_thread.h"

#include "frame_queue.h"
#include "gpu_draw.h"
#include "graphics_probe_render.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <lucent/log.h>

#include "render_retirement.h"

namespace gears
{

namespace
{

std::mutex g_stateMutex;
FrameQueue g_frames;
RenderRetirement g_retirement;
bool g_started = false;
bool g_stopping = false;
uint64_t g_nextFrameId = 0;
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
    // work is allowed to be skipped: at most one newer frame waits, and a later
    // arrival replaces that stale pending frame. The guest's threads have no
    // such freedom -- the audio mixer is a hand-off at 187.5 Hz and every slot
    // it misses is heard -- so when the machine is short of cores, the renderer
    // is the one that should wait. Measured with the renderer at normal priority:
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
        std::optional<QueuedFrame> work = g_frames.WaitTake();
        if (!work.has_value())
            return;

        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t cpu0 = ThreadCpuNanos();
        const uint64_t rq0 = ThreadRunqueueNanos();
        const uint64_t retirementGeneration = work->retirementGeneration;
        auto retire = [retirementGeneration](bool rendered)
        {
            if (rendered)
                g_rendered.fetch_add(1);

            // GPU completion is ordered by the frame-slot owner. Advancing the
            // guest generation here, rather than after CPU submission, keeps
            // EVENT_WRITE_SHD from recycling memory still read by the GPU.
            for (;;)
            {
                RenderRetirement::FinishBatch batch;
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    batch = g_retirement.Finish(retirementGeneration);
                    if (batch.generationComplete)
                        break;
                }
                for (RenderRetirement::Completion &completion : batch.completions)
                    completion();
            }
        };
        if (work->inputs.probe)
            retire(RenderFrameWithGraphicsProbe(work->inputs));
        else
            SubmitFrameRender(work->inputs, std::move(retire));
        g_busyMillis.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - t0)
                                            .count()));
        g_cpuMillis.fetch_add((ThreadCpuNanos() - cpu0) / 1000000ull);
        g_runqueueMillis.fetch_add((ThreadRunqueueNanos() - rq0) / 1000000ull);
        if (!g_frames.Complete(work->frameId))
        {
            lucent::error("draw", "render thread rejected completion for stale frame {}",
                          work->frameId);
            std::terminate();
        }
    }
}

} // namespace

bool SubmitFrameForRender(FrameDrawInputs &&in)
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_submitted.fetch_add(1);
    if (g_stopping)
    {
        g_dropped.fetch_add(1);
        return false;
    }
    if (!g_started)
    {
        g_thread = std::thread(RenderThreadMain);
        g_started = true;
        lucent::info("draw", "render thread started: the command processor hands over"
                             " each frame's draw list and returns; one newer frame may wait while"
                             " rendering, and each newer arrival replaces a stale pending frame");
    }

    const uint64_t frameId = g_nextFrameId++;
    const uint64_t generation = g_retirement.AcceptFrame();
    const FrameQueueSubmitResult result = g_frames.Submit(frameId, generation, std::move(in));
    if (result.status == FrameQueueSubmitStatus::ReplacedPending)
        g_dropped.fetch_add(1);
    if (!result.accepted())
    {
        // Stop and submission are serialized by g_stateMutex, and frame IDs
        // originate here, so reaching this branch means those invariants were
        // broken. The generation was already accepted, so continuing would
        // strand its retirement completions and corrupt the guest GPU contract.
        lucent::error("draw", "render queue rejected monotonic frame {} after acceptance", frameId);
        std::terminate();
    }
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
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_retirement.Defer(completion))
            return;
    }
    completion();
}

void WaitForRenderIdle()
{
    g_frames.WaitIdle();
    WaitForRendererGpuIdle();
}

void StopRenderThread()
{
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (!g_started || g_stopping)
            return;
        g_stopping = true;
        g_frames.Close();
    }
    if (g_thread.joinable())
        g_thread.join();
    WaitForRendererGpuIdle();
}

} // namespace gears
