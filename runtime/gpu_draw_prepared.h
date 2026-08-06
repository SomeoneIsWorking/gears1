#pragma once

// One draw of a frame, as the renderer will issue it.
//
// This used to be declared inside RenderFrameImpl, which meant every phase that
// works on a frame's draw list had to live inside that one function. It is here
// so those phases can be separate translation units -- the EDRAM-tiling collapse
// first.

#include <cstdint>

#include <vulkan/vulkan.h>

namespace gears::draw
{

struct PreparedDraw
{
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet sets[4];
    VkBuffer ibuf;       // VK_NULL_HANDLE for a non-indexed (auto) draw
    VkDeviceSize ibufOffset; // where in the arena this draw's indices live
    uint32_t count;      // index count, or vertex count when !indexed
    bool indexed;
    VkViewport viewport; // the guest's own, per draw
    VkRect2D scissor;
    // Which EDRAM surface this draw renders into. The recording pass below
    // walks these in submission order and re-binds the render pass whenever
    // the surface changes.
    uint32_t surfaceBase = 0;
    // A resolve (RB_MODECONTROL.edram_mode == kCopy) is not geometry: it
    // copies `surface`'s host target out to `resolveDest`'s host image, and
    // issues no draw at all.
    bool isResolve = false;
    uint32_t resolveDest = 0;
    // The guest's resolve rectangle, in EDRAM/surface pixels, and where it
    // lands in the destination texture. Without these a resolve copies the
    // whole surface to the origin, which is why the frame's two predicated
    // tiles overwrote each other instead of assembling (catalog #32).
    VkRect2D resolveSrcRect{};
    int32_t resolveDstX = 0, resolveDstY = 0;
    // RB_COPY_DEST_INFO's copy_dest_exp_bias as a factor, and its
    // copy_dest_swap. Ignoring the bias left the HDR scene texture eight
    // times too bright for the tonemap that samples it (catalog #33).
    float resolveScale = 1.0f;
    bool resolveSwapRB = false;
    // A resolve can also CLEAR the EDRAM it copied out of, and on this
    // title's tiled frame the DEPTH clear rides on each tile's depth
    // resolve -- twice per frame, once per tile. Clearing depth only at the
    // start of the frame leaves the second tile rendering against the first
    // tile's depth buffer.
    bool clearsDepth = false;
    float depthClearValue = 0.0f;
    bool copyIsServed = true;   // false: this entry only clears
    bool resolveIsDepth = false; // a depth resolve, not a colour copy
    // The ColorRenderTargetFormat the copy READS the source EDRAM under, from
    // RB_COLOR_INFO[RB_COPY_CONTROL.copy_src_select] -- NOT RB_COLOR_INFO0.
    // `colorFormat` below is always RT0's, which for a resolve is whatever was
    // last bound and is usually not the surface being copied: every resolve in
    // walk_gameplay.gfr reported format 0 that way, including the bloom copy
    // whose own draws are k_16_16_16_16_FLOAT. Xenia indexes the same four
    // registers (draw_util.cc GetResolveInfo).
    uint32_t resolveSrcFormat = 0;
    // --- mostly diagnostic (GEARS_DRAW_DIAG) ------------------------------
    // EXCEPTION: `blend0` and `colorFormat` ARE read by the renderer. The EDRAM
    // reinterpretation in gpu_draw.cpp uses BlendIsIdentity(blend0) to decide
    // whether a draw reads its destination, and so whether a format change
    // needs converting at all. Do not drop them from a diagnostic-only build.
    // Everything that can make a draw contribute nothing, recorded next to
    // the draw that did nothing. A summary cannot answer "which stage did
    // this surface's draws die at" -- only the join of per-draw state with
    // per-draw pipeline statistics can, and that join is this table.
    uint32_t diagIndex = 0;      // index in the frame's submission order
    uint32_t edramMode = 0;
    uint32_t primType = 0;
    uint64_t vsHash = 0, psHash = 0;
    bool hasFragmentStage = false;
    uint32_t colorMask = 0, depthControl = 0, blend0 = 0;
    uint32_t colorFormat = 0;    // RB_COLOR_INFO color_format
    // RB_COLOR_INFO color_exp_bias, a SIGNED 6-bit exponent the shader's colour
    // output is multiplied by (2^bias) on the way into EDRAM. It scales what a
    // draw writes, so a frame that is uniformly too dim or too bright is a
    // question about this column before it is a question about lighting.
    int32_t colorExpBias = 0;
    // The state that decides whether a primitive survives to rasterisation,
    // which is where this frame's world geometry dies. Raw, so the table
    // shows what the guest programmed rather than our interpretation of it.
    uint32_t clipCntl = 0;       // PA_CL_CLIP_CNTL   0x2204
    uint32_t suScModeCntl = 0;   // PA_SU_SC_MODE_CNTL 0x2205
    uint32_t vteCntl = 0;        // PA_CL_VTE_CNTL    0x206C
    uint32_t windowOffset = 0;   // PA_SC_WINDOW_OFFSET 0x2080
    float vportXScale = 0, vportXOffset = 0, vportYScale = 0, vportYOffset = 0;
    float vportZScale = 0, vportZOffset = 0;
};

} // namespace gears::draw
