#pragma once

// THE FRAME'S RESOLVE ROUTING, worked out before any draw is prepared.
//
// We do not model EDRAM. What CAN be read straight out of the frame's own
// register snapshots is RB_COPY_DEST_BASE (0x2319): the main-memory address the
// guest resolves an EDRAM surface to. A later draw sampling a texture whose
// fetch-constant base equals one of those destinations is sampling this frame's
// own render target -- and that is the entire link between "the draws that
// wrote the scene" and "the passes that read it".
//
// Two subtleties are load-bearing, and both were bugs first:
//
//   RB_COPY_DEST_BASE IS STICKY. Reading it on every draw reports every address
//   any resolve ever set, long after that resolve -- so the scan is restricted
//   to kCopy draws, which are the frame's actual resolves.
//
//   A RESOLVE REPROGRAMS RB_COLOR_INFO to describe its source in whatever
//   format the copy reads it as. Measured on an Act 1 frame, EDRAM base 0x2d0
//   is RENDERED as k_8_8_8_8 but RESOLVED variously as
//   k_2_10_10_10_AS_10_10_10_10, k_2_10_10_10_FLOAT_AS_16_16_16_16, k_16_16 and
//   k_2_10_10_10_FLOAT. That is a REINTERPRETATION of the same bytes, not a
//   different surface, so a resolve's source is looked up by BASE against the
//   surfaces the frame renders -- never by the (base, format) pair its own
//   registers happen to carry.

#include <cstdint>
#include <map>
#include <set>
#include <utility>

#include "gpu_draw.h"
#include "gpu_draw_targets.h"

namespace gears::draw
{

struct ResolvePlan
{
    // Destination texture bases a binding may name to reach this frame's own
    // render target.
    std::set<uint32_t> dests;
    // Depth resolve destinations. Which SHADER samples one is the evidence
    // needed to settle how packed depth is laid out across the destination's
    // four components (catalog #35).
    std::set<uint32_t> depthDests;
    // RB_COPY_DEST_BASE -> (destination texture base, row offset within it).
    // The guest folds a tile's row offset into the base, so two destinations
    // can be two regions of ONE texture.
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> routing;
    // How many colour resolves this frame will DISPATCH. The descriptor pool is
    // sized from this, and it has to be counted HERE rather than from the
    // ResolveEvent list, which is not filled until the draw-preparation loop
    // that runs after -- sizing from an empty list gave 8 sets for 12
    // dispatches, so four silently reused a set a later dispatch overwrote, and
    // the resolves those sets belonged to wrote nothing.
    uint32_t drawCount = 0;
    uint32_t unmatched = 0, fromDepth = 0;
};

// Fills RT.formatsPerBase and returns the routing. Nothing here touches the
// GPU; it is a read of the frame's registers.
ResolvePlan PlanResolves(const FrameDrawInputs& in, RenderTargetCache& RT);

// Both counts are printed even at zero, because "no resolve went unserved" and
// "nobody checked" are the same silence otherwise.
void ReportResolvePlan(const ResolvePlan& plan);

} // namespace gears::draw
