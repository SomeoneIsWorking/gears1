// Rendering a frame off the guest's thread.
//
// WHY. The command processor executes the guest's swap packet from inside the
// guest's own VdSwap call, and the whole-frame render used to happen right there.
// A gameplay frame takes about 45 ms to record and submit, so the guest's render
// thread sat in VdSwap for 45 ms of every frame: measured, VdSwap fell from 29.9
// to 17.9 frames a second the moment gameplay started, and the audio pump -- which
// is paced by a hand-off with a guest thread -- fell behind with it.
//
// The console does not work that way. The GPU consumes the command buffer while
// the CPU runs on; the title synchronises with fences when it needs to. So the
// frame's draw list is handed to this thread and the command processor returns
// immediately.
//
// WHAT THAT COSTS, stated plainly rather than discovered later:
//
//   - The renderer reads guest memory (vertices, indices, textures) WHILE the
//     guest may be writing the next frame into it. On hardware the same race
//     exists and titles handle it with fences; here it means a frame can show a
//     mix of two frames' data. It cannot fault: every read is bounds-checked
//     against the guest window.
//   - One newer frame may wait while the renderer is busy, keeping the renderer
//     saturated without an unbounded latency queue. A newer arrival replaces a
//     stale pending frame; the displaced frame is counted and reported.
//   - The register snapshots the draw list points at are shared_ptr copies, and
//     the microcode pointers are into the shader-capture map, whose entries are
//     never erased. Those are safe to read from this thread; the guest's PIXEL
//     data is what races.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

#include "gpu_draw.h"

namespace gears
{

// Hand one frame's draw list to the render thread. One frame may wait behind the
// one being rendered; a newer arrival replaces that pending frame without
// waiting. Returns false only after shutdown. Starts the thread on first use.
bool SubmitFrameForRender(FrameDrawInputs &&in);

// Frames handed over, and pending frames displaced by newer arrivals.
struct RenderThreadStats
{
    uint64_t submitted = 0;
    uint64_t dropped = 0;
    uint64_t rendered = 0;
    uint64_t busyMillis = 0;     // wall time preparing/submitting frames on the CPU
    uint64_t cpuMillis = 0;      // ...of which this thread actually ran
    uint64_t runqueueMillis = 0; // ...and spent RUNNABLE but off-core
    bool gpuTimingAvailable = false;
    uint64_t gpuSamples = 0;
    uint64_t gpuNanoseconds = 0;
    uint64_t gpuMaximumNanoseconds = 0;
    uint64_t gpuFailedSamples = 0;
};
RenderThreadStats RenderThreadCounters();

// Owns the once-per-second live renderer report and its interval baselines.
// Keeping this beside RenderThreadStats prevents the command processor from
// acquiring renderer timing policy and formatting.
class RenderThreadReporter
{
  public:
    void MaybeReport();

  private:
    std::chrono::steady_clock::time_point lastReport_ = std::chrono::steady_clock::now();
    uint64_t lastRendered_ = 0;
    uint64_t lastDropped_ = 0;
    uint64_t lastBusyMillis_ = 0;
    uint64_t lastCpuMillis_ = 0;
    uint64_t lastRunqueueMillis_ = 0;
    uint64_t lastGpuSamples_ = 0;
    uint64_t lastGpuNanoseconds_ = 0;
    uint64_t lastGpuFailedSamples_ = 0;
};

// Publish a completion only after the frame currently owned by the renderer
// has retired. Runs the completion immediately when no frame is in flight.
// Unlike WaitForRenderIdle, this never blocks the caller: GPU retirement
// packets delay their memory write, not the command processor consuming them.
void DeferUntilAcceptedRenderRetires(std::function<void()> completion);

// Wait for any in-flight frame to finish. Capture/report paths use this before
// reading renderer output; live GPU retirement uses
// DeferUntilAcceptedRenderRetires so the command processor remains asynchronous.
void WaitForRenderIdle();

// Stop after the in-flight frame and its already accepted waiting frame. Idempotent.
void StopRenderThread();

} // namespace gears
