#pragma once

// The render-target cache: one host colour target per EDRAM surface, one host
// image per resolve destination, and the (clear, load) render-pass pair each
// host colour format needs.
//
// The console has 10 MiB of EDRAM addressed by a base register; a host renderer
// has images. Everything here is that translation, and the reasons live with the
// functions in gpu_draw_targets.cpp -- they are the reasons a frame assembles or
// overwrites itself.
//
// This was 420 lines of lambdas in the middle of RenderFrameImpl. The counters
// stay public because the frame report prints them: a resolve this cache refuses
// to serve is a resolve that did not happen, and one that is not reported looks
// exactly like one that was not asked for.

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "gpu_draw_renderer.h"

namespace gears::draw
{

struct RenderTargetCache
{
    RenderTargetCache(Renderer& r, RendererPersistent& p, const FrameDrawInputs& inputs,
                      uint32_t w, uint32_t h, VkFormat depthFmt, VkImageView depthImageView)
        : R(r), P(p), in(inputs), W(w), H(h),
          depthFormat(depthFmt), depthView(depthImageView) {}

    Renderer& R;
    RendererPersistent& P;
    const FrameDrawInputs& in;
    const uint32_t W, H;
    const VkFormat depthFormat;
    const VkImageView depthView;


        std::map<uint32_t, std::set<uint32_t>> formatsPerBase;

    std::vector<VkDescriptorSet> resolveSets;
    uint32_t resolveSetsUsed = 0;
    // The depth resolve's sets must be allocated with the DEPTH layout: its
    // binding 0 is a SAMPLED image where the colour layout has a STORAGE one.
    // Borrowing a colour set and binding it to the depth pipeline writes a
    // descriptor of the wrong type into it -- which does not fail, it just
    // reads nothing.
    std::vector<VkDescriptorSet> resolveDepthSets;
    uint32_t resolveDepthSetsUsed = 0;
    // EDRAM format reinterpretation (gpu_draw_reinterpret.cpp): one descriptor
    // set per conversion, and counters for what was converted, what was refused
    // as an unsupported format pair, and what ran out of sets. A refusal is not
    // a no-op -- the surface keeps a value the console never held -- so it is
    // counted separately and reported at Warn.
    std::vector<VkDescriptorSet> reinterpretSets;
    uint32_t reinterpretSetsUsed = 0;
    uint32_t reinterpretsDone = 0;
    uint32_t reinterpretsRefused = 0;
    uint32_t reinterpretsOutOfSets = 0;
    std::set<uint64_t> reinterpretPairs;        // (from << 32) | to
    std::set<uint64_t> reinterpretRefusedPairs;

    uint32_t resolvesUnstorable = 0;
    uint32_t resolvesOutOfSets = 0;
    uint32_t midFrameDepthClears = 0;

    uint32_t resolveNoFormat = 0;
    // Resolves whose rectangle could not be read from vf0.
    uint32_t resolveNoRect = 0;
    // Bumped when a NEW resolve target appears. A cached set names image views,
    // and a view is a handle: a resolve re-running writes new CONTENTS through the
    // same handle, which needs no invalidation. What does invalidate is a
    // destination being seen for the first time mid-frame, because a draw before
    // that point resolved the same binding to a stub or to guest memory.
    uint64_t resolveGeneration = 0;


// Two variants of the same pass per host colour format: one that CLEARS the
// surface (its first use in a frame) and one that LOADS it (every use
// after, so the console's predicated tiles accumulate). The frame is also
// split at every surface change and at every resolve, because a surface can
// only be blitted out while no pass is open on it.
    bool MakeRenderPass(VkFormat colorFormat, bool load, VkRenderPass& out);

// One (clear, load) pair per host colour format, created on first use.
    bool GetPasses(VkFormat colorFormat, std::pair<VkRenderPass, VkRenderPass>*& out);

// Every RB_COLOR_INFO.color_format the frame renders each EDRAM base with,
// filled by the pre-pass further down and read by getSurfaceTarget, which
// sizes one host image per base wide enough to hold all of them.
    bool GetSurfaceTarget(uint32_t base, SurfaceTarget*& out);

// The host image a resolve destination owns.
//
// It is always R16G16B16A16_SFLOAT, whatever its source surface's format
// is, and the resolve is a BLIT rather than a copy. That is not laziness:
// measured on an Act 1 frame, destination 0xbde0000 receives resolves from
// BOTH the 8888 tonemap surface at 0x2d0 and the 7e3 HDR surface at 0x400,
// so no single source format describes it. A wide float destination holds
// an 8888 UNORM value (0..1) exactly and a 7e3 HDR value (0..32) without
// clamping, and vkCmdBlitImage does the conversion; an 8888 destination
// would clamp every HDR highlight the tonemap pass exists to read.
    bool GetResolveTarget(uint32_t destBase, uint32_t sourceBase, uint32_t destPitch,
                          uint32_t destHeight, uint32_t destFormat, bool isDepth,
                          ResolveTarget*& out, uint32_t& rowOffsetOut);

    // --- EDRAM format reinterpretation, in gpu_draw_reinterpret.cpp ---------
    // Built once. Returns false, having said why, when the device or the
    // pipeline cannot serve it -- a frame that then changes format is reported
    // as unconverted rather than silently rendering on.
    bool BuildReinterpretPipeline();
    // Convert a surface's contents from one guest colour format to another, in
    // place, through the bits the console's EDRAM would hold. Recorded with no
    // render pass open. Returns false when the pair is one this pass refuses.
    bool ReinterpretSurface(VkCommandBuffer cmd, SurfaceTarget& t,
                            uint32_t fromFormat, uint32_t toFormat);
    // GEARS_DRAW_REINTERP_SELFTEST=1: run the shipping compute module on this
    // GPU against three cases with arithmetic answers -- two that must convert
    // and one that must NOT change anything -- before any frame is rendered.
    bool ReinterpretSelfTest();
    // The frame's line. `enabled` false still prints, naming how many surfaces
    // changed format with the pass off: silence would read as "nothing to do".
    void ReportReinterpretation(bool enabled) const;

    // Built once, on the first frame that can have it: the resolve is a DISPATCH
    // and not a vkCmdBlitImage because it applies the guest's copy_dest_exp_bias
    // and copy_dest_swap, which a blit cannot do (catalog #33).
    void BuildResolvePipeline();

    // The two resolves themselves, in gpu_draw_resolve.cpp. Both are recorded
    // into an open command buffer with no render pass active, and both leave
    // the destination in SHADER_READ_ONLY_OPTIMAL so a later draw can sample it.

    // Reads the host depth image through a sampled view and writes the depth the
    // guest's k_24_8_FLOAT fetch would produce.
    void ResolveDepthTo(VkCommandBuffer cmd, ResolveTarget& dst,
                        const VkRect2D& srcRect, int32_t dstX, int32_t dstY);

    // Colour. `scale` is the guest's copy_dest_exp_bias applied as a multiplier
    // and `swapRB` its copy_dest_swap -- neither of which the blit control arm
    // (GEARS_DRAW_RESOLVE_BLIT=1) can do.
    void ResolveSurfaceTo(VkCommandBuffer cmd, const SurfaceTarget& srcTarget,
                          ResolveTarget& dst, const VkRect2D& srcRect,
                          int32_t dstX, int32_t dstY, float scale, bool swapRB);
};

} // namespace gears::draw
