#pragma once

// --- collapse the console's predicated EDRAM tiling ------------------
// GEARS_DRAW_UNTILE=1. THIS IS THE FIRST STEP THAT IS ACTUALLY A NATIVE
// RENDERER RATHER THAN AN EMULATOR, so it is worth saying what it does and
// why the console did the other thing.
//
// The Xbox 360 has 10 MiB of EDRAM. A 1280x720 colour+depth surface does not
// fit, so UE3-on-360 splits it into TILES and replays the whole command
// buffer once per tile: the same draws, the same shaders, the same geometry,
// with a different scissor band and a PA_SC_WINDOW_OFFSET that shifts the
// world so the tile's rows land at the top of EDRAM. Each tile is then
// resolved out to its own rows of the destination texture.
//
// MEASURED on the Act 1 courtyard frame: the base pass appears twice, 174
// draws each, and 40 of the 46 columns of the per-draw table are IDENTICAL
// across all 174 pairs -- same pixel shader, same vertex shader, same index
// count, same blend, same depth state, same surface. Only three PROGRAMMED
// values differ: viewport height (720 vs 208), scissor height (512 vs 208)
// and the window offset (0 vs y = -512). The other three differences are
// outcomes, not inputs (primitives surviving clip, fragments, the verdict).
//
// A host renderer targeting a full-resolution image has no 10 MiB budget and
// no reason to do any of it. Tile 0 already carries the FULL viewport height
// and a window offset of zero -- only its scissor clips it to the first band
// -- so widening that scissor to the whole surface draws the identical
// picture in ONE pass. The replay tiles are then redundant and are dropped,
// and their resolves collapse into one covering the union.
//
// WHAT IT REFUSES TO DO. It collapses only when the replay is provably a
// replay: the same number of draws, in the same order, with the same
// (vertex shader, pixel shader, index count, primitive type, colour mask,
// blend, depth control) on every one of them. Anything else is left alone
// and reported, because a "collapse" that silently dropped draws the guest
// meant differently would look like a performance win and be a corruption.
//
// Returns the number of draws it dropped, and subtracts them from `issued`.

#include <cstdint>
#include <vector>

#include "gpu_draw_prepared.h"

namespace gears::draw
{

void CollapseEdramTiling(std::vector<PreparedDraw>& prepared, uint32_t& issued);

} // namespace gears::draw
