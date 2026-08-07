#pragma once

// What the frame CONTAINED, as distinct from what the renderer did with it.
//
// Every counter here answers a question that the picture alone cannot. "The
// frame got darker" and "36% of the frame's draws stopped writing colour they
// should never have written" are the same observation, and only a number tells
// them apart; a draw that adds no pixels is geometry killed at clip, a
// rasterised nothing, or a shader that ran and blended to nothing, and only a
// tally separates those.
//
// They live together because they are reported together, and because holding
// them as a dozen loose locals in the frame function is what let one of them be
// declared and never accumulated -- 18 ms of a 36 ms draw loop with no name.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "gpu_draw.h"

namespace gears::draw
{

struct FrameCensus
{
    uint32_t issued = 0, skipped = 0;

    // Per-EDRAM-surface. A draw's RB_COLOR_INFO (0x2001) names the EDRAM tile
    // base it renders into; distinct bases are distinct surfaces (scene colour,
    // light attenuation, shadow depth, the post chain...). UE3-on-360 renders
    // each surface in predicated TILES, so several draws share a base.
    //
    // The mode breakdown is PER SURFACE, not just per frame: "353 draws target
    // the HDR world surface" and "how many of those can write colour at all"
    // are different questions, and only the second explains an empty surface.
    struct SurfaceStat
    {
        uint32_t draws = 0; uint32_t format = 0; uint32_t mode = 0;
        uint32_t colorDepth = 0, depthOnly = 0, otherMode = 0;
    };
    std::map<uint32_t, SurfaceStat> surfaces; // RB_COLOR_INFO color_base -> stat

    // RB_MODECONTROL.edram_mode (0x2208, bits 0..2) per draw. This is NOT a
    // detail: on Xenos a draw with edram_mode == kCopy (6) is not geometry at
    // all, it is a RESOLVE -- the primitive selects the region of the EDRAM
    // surface to copy out to main memory (Xenia: VulkanCommandProcessor::
    // IssueDraw dispatches straight to IssueCopy on that mode). Rendering one
    // as if it were geometry paints the resolve rectangle's shader output into
    // the colour target. Counted before anything acts on it.
    //   0 kNoOperation  4 kColorDepth  5 kDepthOnly  6 kCopy
    std::map<uint32_t, uint32_t> edramModes;

    // Draws issued with NO fragment stage because their edram_mode is not
    // kColorDepth.
    uint32_t drawsNoPixelShader = 0;
    // Draws Xenia would not issue at all -- IsRasterizationPotentiallyDone is
    // false, so the draw covers nothing. Counted rather than dropped silently:
    // a frame where this is large and the picture is empty has a very different
    // cause from one where the draws were issued and shaded nothing.
    uint32_t drawsNoRasterisation = 0;
    // Draws whose pixel shader could not be analysed, so the fragment-stage
    // question was left UNDECIDED and the mode test alone decided it. Not zero
    // by construction, and a non-zero value means the numbers above are a
    // partial answer -- which is why it is reported next to them.
    uint32_t drawsClassifyFailed = 0;

    std::set<uint32_t> depthBases;   // distinct RB_DEPTH_INFO.depth_base
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> surfaceDepthPairs;
    uint32_t issuedResolves = 0, skippedResolves = 0;

    // Reason code -> count. Skip() takes the code so a `continue` cannot leave
    // the two apart -- every skipped draw is a draw missing from the picture,
    // and one that is not attributed is one nobody can look for.
    std::map<uint64_t, uint64_t> skipReasons;
    void Skip(uint64_t reason) { ++skipped; ++skipReasons[reason]; }

    // Geometry reach: how many draws fetch vertices from outside the SSBO
    // mirror. Such a fetch reads zero, so every primitive collapses -- and the
    // result looks exactly like "shaded black", which is why it is counted.
    uint64_t vfDrawsPastMirror = 0, vfDrawsInMirror = 0;
    uint32_t vfHighestByte = 0;

    std::map<std::string, uint64_t> viewportCensus; // guest viewport/scissor -> draws

    // The per-draw tallies that every draw contributes to, resolves included,
    // taken before any early exit so the census covers the whole frame.
    void NoteDraw(uint32_t surfaceBase, uint32_t colorFormat, uint32_t edramMode);
    void NoteDepth(uint32_t surfaceBase, uint32_t depthBase);

    // The report lines this owns. The render-target cache's own lines are
    // printed by the caller between these, because they interleave.
    void ReportSurfaces() const;
    void ReportModes() const;
    void ReportDepthPairs() const;
    void ReportReach(uint64_t mirrorBytes) const;
    void ReportViewports() const;
    void ReportSkips(size_t drawCount) const;
};

} // namespace gears::draw
