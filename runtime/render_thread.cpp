#include "render_thread.h"

#include "frame_queue.h"
#include "gpu_draw.h"
#include "gpu_frame_timing.h"
#include "graphics_probe_render.h"
#include "frame_production_timing.h"
#include "rhi_renderer_input.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <format>
#include <mutex>
#include <thread>
#include <utility>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <lucent/log.h>
#include <lucent/config.h>

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
std::atomic<uint64_t> g_busyNanoseconds{0};
// Wall against CPU, the same discriminator the audio pump uses (catalog #43): a
// renderer that is genuinely slow burns thread CPU time roughly equal to its wall
// time, while one that is descheduled shows wall far above CPU. The live frame
// costs 53 ms against 29 ms for the same frame replayed offline, and those two
// explanations want opposite fixes.
std::atomic<uint64_t> g_cpuNanoseconds{0};
std::atomic<uint64_t> g_runqueueNanoseconds{0};

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
        g_busyNanoseconds.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now() - t0)
                                                 .count()));
        g_cpuNanoseconds.fetch_add(ThreadCpuNanos() - cpu0);
        g_runqueueNanoseconds.fetch_add(ThreadRunqueueNanos() - rq0);
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
        if (in.sequence >= 0)
            ObserveRhiRendererFrameDropped(static_cast<uint64_t>(in.sequence));
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
    {
        g_dropped.fetch_add(1);
        if (result.displacedFrameSequence.has_value())
            ObserveRhiRendererFrameDropped(*result.displacedFrameSequence);
    }
    if (!result.accepted())
    {
        // Stop and submission are serialized by g_stateMutex, and frame IDs
        // originate here, so reaching this branch means those invariants were
        // broken. The generation was already accepted, so continuing would
        // strand its retirement completions and corrupt the guest GPU contract.
        lucent::error("draw", "render queue rejected monotonic frame {} after acceptance", frameId);
        std::terminate();
    }
    static const bool traceEnabled = lucent::config::flag("FRAME_PRODUCTION_TRACE");
    if (traceEnabled)
        (void)GlobalFrameProductionTiming().Observe(FrameProductionStage::RenderSubmission,
                                                    FrameProductionTiming::Clock::now());
    return true;
}

RenderThreadStats RenderThreadCounters()
{
    RenderThreadStats out;
    out.submitted = g_submitted.load();
    out.dropped = g_dropped.load();
    out.rendered = g_rendered.load();
    out.busyMillis = g_busyNanoseconds.load() / 1000000ull;
    out.cpuMillis = g_cpuNanoseconds.load() / 1000000ull;
    out.runqueueMillis = g_runqueueNanoseconds.load() / 1000000ull;
    const draw::GpuFrameTimingStats gpu = draw::CurrentGpuFrameTimingStats();
    out.gpuTimingAvailable = gpu.available;
    out.gpuSamples = gpu.samples;
    out.gpuNanoseconds = gpu.totalNanoseconds;
    out.gpuMaximumNanoseconds = gpu.maximumNanoseconds;
    out.gpuFailedSamples = gpu.failedSamples;
    return out;
}

void RenderThreadReporter::MaybeReport()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport_ < std::chrono::seconds(1))
        return;

    const RenderThreadStats st = RenderThreadCounters();
    const double elapsed = std::chrono::duration<double>(now - lastReport_).count();
    const uint64_t rendered = st.rendered - lastRendered_;
    const uint64_t gpuSamples = st.gpuSamples - lastGpuSamples_;
    const std::string gpuReport =
        !st.gpuTimingAvailable ? "unavailable"
        : gpuSamples == 0      ? "no completed samples in this interval"
                          : std::format("{:.2f} ms/frame over {} interval completions"
                                        " (process max {:.2f} ms; {} interval failures)",
                                        double(st.gpuNanoseconds - lastGpuNanoseconds_) /
                                            1000000.0 / double(gpuSamples),
                                        gpuSamples, double(st.gpuMaximumNanoseconds) / 1000000.0,
                                        st.gpuFailedSamples - lastGpuFailedSamples_);
    lucent::info("gpu",
                 "guest-draw: {:.1f} frames/s rendered, {:.1f}/s dropped as the one-frame"
                 " renderer queue was full ({} ms/frame in RenderFrame, of which {} ms"
                 " on-core and {} ms runnable-but-off-core); GPU {}",
                 double(rendered) / elapsed, double(st.dropped - lastDropped_) / elapsed,
                 (st.busyMillis - lastBusyMillis_) / std::max<uint64_t>(1, rendered),
                 (st.cpuMillis - lastCpuMillis_) / std::max<uint64_t>(1, rendered),
                 (st.runqueueMillis - lastRunqueueMillis_) / std::max<uint64_t>(1, rendered),
                 gpuReport);
    lastReport_ = now;
    lastRendered_ = st.rendered;
    lastDropped_ = st.dropped;
    lastBusyMillis_ = st.busyMillis;
    lastCpuMillis_ = st.cpuMillis;
    lastRunqueueMillis_ = st.runqueueMillis;
    lastGpuSamples_ = st.gpuSamples;
    lastGpuNanoseconds_ = st.gpuNanoseconds;
    lastGpuFailedSamples_ = st.gpuFailedSamples;
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
