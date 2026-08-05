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

    // Built once, on the first frame that can have it: the resolve is a DISPATCH
    // and not a vkCmdBlitImage because it applies the guest's copy_dest_exp_bias
    // and copy_dest_swap, which a blit cannot do (catalog #33).
    void BuildResolvePipeline();
};

} // namespace gears::draw
