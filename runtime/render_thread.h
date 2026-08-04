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
//   - A frame that arrives while the renderer is still busy is DROPPED, not
//     queued. Queueing would trade latency for nothing -- the dropped frame's
//     successor is already more current -- but a silent drop would make a
//     renderer running at half rate look like one keeping up, so drops are
//     counted and reported.
//   - The register snapshots the draw list points at are shared_ptr copies, and
//     the microcode pointers are into the shader-capture map, whose entries are
//     never erased. Those are safe to read from this thread; the guest's PIXEL
//     data is what races.
#pragma once

#include <cstdint>

#include "gpu_draw.h"

namespace gears
{

// Hand one frame's draw list to the render thread. Returns true if it was taken,
// false if the renderer was still busy with the previous one (the frame is
// dropped). Starts the thread on first use.
bool SubmitFrameForRender(FrameDrawInputs&& in);

// Frames handed over, and frames dropped because the renderer was busy.
struct RenderThreadStats
{
    uint64_t submitted = 0;
    uint64_t dropped = 0;
    uint64_t rendered = 0;
    uint64_t busyMillis = 0;   // wall time inside RenderFrame
    uint64_t cpuMillis = 0;    // ...of which this thread actually ran
    uint64_t runqueueMillis = 0; // ...and spent RUNNABLE but off-core
};
RenderThreadStats RenderThreadCounters();

// Wait for any in-flight frame to finish. For a run that is about to write a
// report or a screenshot from the renderer's own output.
void WaitForRenderIdle();

// Stop the thread after the current frame. Idempotent.
void StopRenderThread();

} // namespace gears
