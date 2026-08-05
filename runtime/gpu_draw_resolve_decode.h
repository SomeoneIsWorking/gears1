#pragma once

// A kCopy draw is NOT geometry. On Xenos it copies an EDRAM surface out to main
// memory and the primitive only selects the region, so issuing one as a draw
// paints the resolve rectangle's shader output over the surface. This is where
// such a draw is decoded into the PreparedDraw it really is.
//
// gpu_draw_targets.h owns the resolve TARGETS and gpu_draw_resolve.cpp the two
// dispatches that fill them; this is the front half -- reading the guest's
// registers to work out what is being resolved, from where, to where.

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "gpu_draw.h"
#include "gpu_draw_census.h"
#include "gpu_draw_prepared.h"
#include "gpu_draw_targets.h"

namespace gears::draw
{

// The guest's resolve rectangle, per Xenia's GetResolveInfo. Shared by the
// colour and depth paths -- a depth resolve carries the same rectangle and the
// same window offset as the colour resolve of its tile. A rectangle that cannot
// be read from vf0 leaves the full target extent and counts RT.resolveNoRect,
// because a silently full-screen resolve looks exactly like a correct one.
void DeriveResolveRect(const uint32_t* R, const FrameDrawInputs& in,
                       uint32_t W, uint32_t H, RenderTargetCache& RT,
                       int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1);

// Decodes one kCopy draw and appends whatever PreparedDraws it implies -- a
// colour resolve, a depth resolve, a depth clear, or nothing but a census
// entry. `routing` maps a destination base to (texture base, row offset): the
// guest folds a tile's row offset into RB_COPY_DEST_BASE, so two destinations
// can be two regions of ONE texture.
void PrepareResolveDraw(const uint32_t* R, const FrameDrawItem& d,
                        const FrameDrawInputs& in, uint32_t W, uint32_t H,
                        const std::map<uint32_t, std::pair<uint32_t, uint32_t>>& routing,
                        RenderTargetCache& RT, FrameCensus& CN,
                        std::vector<PreparedDraw>& prepared);

} // namespace gears::draw
