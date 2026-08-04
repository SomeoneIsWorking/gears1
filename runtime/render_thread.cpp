#include "render_thread.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <lucent/log.h>

namespace gears
{

namespace
{

std::mutex g_mutex;
std::condition_variable g_wake;      // renderer waits for work
std::condition_variable g_idle;      // WaitForRenderIdle waits for the renderer
std::optional<FrameDrawInputs> g_pending;
bool g_rendering = false;
bool g_stop = false;
bool g_started = false;
std::thread g_thread;

std::atomic<uint64_t> g_submitted{0};
std::atomic<uint64_t> g_dropped{0};
std::atomic<uint64_t> g_rendered{0};
std::atomic<uint64_t> g_busyMillis{0};

void RenderThreadMain()
{
    // NICED DOWN, deliberately. This thread is the only one in the process whose
    // work is allowed to be skipped: a frame it cannot render in time is dropped
    // and the next one is more current anyway. The guest's threads have no such
    // freedom -- the audio mixer is a hand-off at 187.5 Hz and every slot it
    // misses is heard -- so when the machine is short of cores, the renderer is
    // the one that should wait. Measured with the renderer at normal priority:
    // the guest's frame loop recovered to 28 fps but the audio pump fell to
    // 57-67 Hz against 187.5.
    //
    // setpriority on a thread id, which on Linux is what the scheduler actually
    // weights; it needs no privileges to lower a priority.
    const int tid = int(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, id_t(tid), 5) != 0)
        lucent::warn("draw", "render thread: could not nice down (errno {});"
            " it will compete with the guest's audio for cores", errno);

    for (;;)
    {
        FrameDrawInputs work;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait(lock, [] { return g_pending.has_value() || g_stop; });
            if (g_stop && !g_pending.has_value())
                return;
            work = std::move(*g_pending);
            g_pending.reset();
            g_rendering = true;
        }

        const auto t0 = std::chrono::steady_clock::now();
        RenderFrame(work);
        g_busyMillis.fetch_add(uint64_t(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count()));
        g_rendered.fetch_add(1);

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_rendering = false;
        }
        g_idle.notify_all();
    }
}

} // namespace

bool SubmitFrameForRender(FrameDrawInputs&& in)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_started)
    {
        g_started = true;
        g_thread = std::thread(RenderThreadMain);
        lucent::info("draw", "render thread started: the command processor hands over"
            " each frame's draw list and returns, so the guest's VdSwap no longer"
            " waits for the render");
    }
    g_submitted.fetch_add(1);
    // BUSY MEANS DROP, not queue. A queued frame is already stale by the time it
    // would be drawn, and queueing hides the deficit; dropping shows it in the
    // counter and keeps latency at one frame.
    if (g_rendering || g_pending.has_value())
    {
        g_dropped.fetch_add(1);
        return false;
    }
    g_pending = std::move(in);
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
    return out;
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
