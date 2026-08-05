#pragma once

// The frame's two mid-render probes, both of which answer the same question:
// WHICH DRAW PAINTED THIS?
//
//   GEARS_DRAW_FRAME_STEP=N        every N draws, the whole colour target
//   GEARS_DRAW_PIXEL_TRACE=<x>,<y> every draw, one texel
//
// They are here rather than in the frame function because both are recorded
// mid-frame and reported at the end, and holding a probe's two halves 1300
// lines apart is how the checkpoint knob spent its whole life passing a null
// target and writing nothing while looking exactly like a frame with nothing
// to report.

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "gpu_draw_prepared.h"
#include "gpu_draw_renderer.h"

namespace gears::draw
{

struct FrameProbe
{
    FrameProbe(Renderer& r, uint32_t w, uint32_t h) : R(r), W(w), H(h) {}

    Renderer& R;
    const uint32_t W, H;

    // Reads the knobs and creates what each enabled probe needs. nDraws is the
    // frame's prepared-draw count, which bounds the pixel trace's buffer.
    // A probe that cannot be set up says so and stays off -- it never falls
    // back to recording nothing quietly.
    void Build(size_t nDraws, VkDeviceSize readbackBytes);

    // True when a checkpoint is due after `drawn` draws. Kept here so the knob
    // and the cadence it implies cannot drift apart.
    bool CheckpointDue(uint32_t drawn) const;
    bool Tracing() const { return traceX >= 0; }

    // Both must be called with NO render pass open -- an image copy cannot
    // happen inside one -- and with the target read BEFORE the pass was ended,
    // since ending it nulls the caller's openTarget.
    void Checkpoint(VkCommandBuffer cmd, uint32_t drawsSoFar, const SurfaceTarget* t,
                    uint32_t surfaceBase);
    void TracePixel(VkCommandBuffer cmd, uint32_t drawsSoFar, const SurfaceTarget* t,
                    uint32_t surfaceBase);

    // Writes the checkpoint images and prints the trace. Call after the frame's
    // fence has been waited on; `prepared` names the draw behind each sample.
    void Report(const std::vector<PreparedDraw>& prepared);

    // Frees everything Build created. After Report, and after the fence.
    void Release();

private:
    // Each checkpoint costs a full-frame readback buffer, so STEP=1 on a
    // 170-draw frame is capped rather than allocating 170 of them.
    static constexpr size_t kMaxCheckpoints = 48;

    // draws-so-far -> (buffer, EDRAM surface base). THE SURFACE IS NOT OPTIONAL:
    // a checkpoint dumps whichever surface is bound at that moment, and a frame
    // switches between several. Without the base in the line, a checkpoint that
    // goes from 900k non-black pixels to zero reads as "something wiped the
    // frame" when it is only the render target changing to a small bloom buffer
    // -- which is exactly how it was misread once.
    struct Checkpt { uint32_t draws; VkBuffer buffer; uint32_t surface; };
    struct PixelSample { uint32_t draws; uint32_t surface; VkFormat format; };

    long stepEvery = 0, stepFrom = 0;
    VkDeviceSize rbBytes = 0;
    std::vector<Checkpt> checkpoints;
    std::vector<VkDeviceMemory> checkpointMem;
    uint32_t checkpointsSkipped = 0;
    // Its OWN staging image, not one of the present pair: those are the frames
    // the presenter thread may still be blitting, and a diagnostic must never be
    // able to corrupt what the user sees.
    VkImage checkpointStage = VK_NULL_HANDLE;
    VkDeviceMemory checkpointStageMem = VK_NULL_HANDLE;

    std::vector<PixelSample> pixelSamples;
    VkBuffer pixelBuf = VK_NULL_HANDLE;
    VkDeviceMemory pixelMem = VK_NULL_HANDLE;
    int32_t traceX = -1, traceY = -1;
};

// Per-draw pipeline statistics, and the diagnostic table built on them.
//
// Four counters per draw: input-assembly vertices, input-assembly primitives,
// clipping primitives (what survived clip+cull) and fragment invocations. A
// draw that adds no pixels is one of three very different things, and only
// these numbers separate them: 0 primitives out of clipping (degenerate or
// culled geometry), 0 fragment invocations (rasterised nothing), or many
// fragment invocations (it ran and shaded/blended to nothing).
struct DrawStats
{
    DrawStats(Renderer& r) : R(r) {}

    Renderer& R;

    // Reads the knobs and creates the query pool. `nDraws` bounds it.
    // GEARS_DRAW_DIAG=<path.tsv> writes the table, which is only useful joined
    // with the statistics -- so it turns them on rather than making the user
    // remember two knobs. Not combinable with DRAW_ONLY: unwritten queries
    // would never resolve.
    void Build(VkCommandBuffer cmd, size_t nDraws);
    bool Enabled() const { return pool != VK_NULL_HANDLE; }

    void Begin(VkCommandBuffer cmd, uint32_t drawIndex);
    void End(VkCommandBuffer cmd, uint32_t drawIndex);

    // Reads the results back, prints the frame summary and writes the table.
    // `drawn` is how many queries were actually recorded. Destroys the pool.
    void Report(uint32_t drawn, const std::vector<PreparedDraw>& prepared);

private:
    // One row per issued draw, joining what the draw WAS (surface, EDRAM mode,
    // primitive, shaders) with what it DID (pipeline statistics) and with every
    // piece of state that can silently zero it. This table replaces guessing:
    // "surface 0x400 renders nothing" is not actionable, "all 348 of surface
    // 0x400's colour draws report primitives in and zero primitives after
    // clip+cull, and every one has depth func GEQUAL against a cleared 1.0
    // depth buffer" names the defect. Grouping and filtering belong to whatever
    // reads the TSV, so the renderer stays out of the analysis business.
    void WriteTable(uint32_t drawn, const std::vector<PreparedDraw>& prepared,
                    const std::vector<uint64_t>& st);

    static constexpr uint32_t kCounters = 4;
    VkQueryPool pool = VK_NULL_HANDLE;
    std::string diagPath;
};

} // namespace gears::draw
