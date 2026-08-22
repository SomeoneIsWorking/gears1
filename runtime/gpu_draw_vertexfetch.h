#pragma once

// What a draw FETCHES from guest memory, and saying so.
//
// A vertex fetch constant names a byte range in the guest's physical window.
// Two things follow from that range and both live here:
//
//   * it is exactly the memory that must be uploaded into the SSBO mirror, so
//     the frame's upload list is built from it;
//   * a fetch that reaches PAST the mirror reads zero, which collapses every
//     primitive to the origin and is destroyed at clipping -- and the result
//     looks exactly like "shaded black". A 64 MiB mirror against an Act 1
//     frame's 237 MiB reach cost 606 of 722 draws that way (catalog #30), and
//     it still looked like a frame. That is why reach is counted rather than
//     assumed.

#include <cstdint>
#include <utility>
#include <vector>

#include "gpu_draw.h"
#include "gpu_draw_census.h"
#include "gpu_draw_uniforms.h"
#include "gpu_draw_xlate.h"

namespace gears::draw
{

// Walks both shaders' vertex bindings, tallies this draw's geometry reach into
// the census, and appends every range the GPU will read to `fetchRanges`.
//
// The PIXEL shader's fetches are uploaded but NOT counted as geometry reach:
// they are memory the GPU reads, but whether they resolve says nothing about
// whether this draw's geometry does, and conflating the two makes a reach
// number that cannot be acted on.
void CollectFetchRanges(const uint32_t* R, const FrameDrawInputs& in,
                        const ShaderXlate& vsX, const ShaderXlate& psX,
                        FrameCensus& CN,
                        std::vector<std::pair<uint64_t, uint64_t>>& fetchRanges);

// GEARS_DRAW_VDUMP=<draw index>: dump that draw's first vertices out of the
// mirror, as the vertex shader's own fetch describes them (stride from the
// shader's vertex binding, big-endian floats). Says whether a draw that
// rasterises but shades black is fed real vertex data.
//
// `diagIndex` is the index the per-draw diagnostic table's `draw` column
// reports -- position in the frame's submission order, NOT a count of draws
// issued so far. Those diverge wherever a draw is a resolve or is skipped, and
// reading an index off the table only to dump a different draw is a trap worth
// not having. Takes a comma-separated list, so two draws can be compared from
// one run.
void DumpVertices(const uint32_t* R, const FrameDrawInputs& in,
                  const FrameDrawItem& draw, const ShaderXlate& vsX,
                  const ShaderXlate& psX,
                  uint32_t issued, uint32_t diagIndex,
                  uint64_t vsHash = 0, uint32_t vertexCount = 0,
                  const std::vector<uint8_t>* systemConstants = nullptr,
                  const std::vector<uint8_t>* vertexConstants = nullptr);

// GEARS_DRAW_VS_CONSTS=<draw index>[,<draw index>...]: the VERTEX float
// constants those draws actually received, as numbers and as raw bits.
//
// GEARS_DRAW_PS_CONSTS keys on a SHADER HASH, which cannot separate repeated
// instances of one mesh -- and that is precisely the case this answers: when
// several draws share a shader pair, an index count and byte-identical raster
// state but only some of them survive clipping, the per-instance transform in
// these constants is the remaining difference (catalog #74).
void DumpVsConstants(const ShaderXlate& vsX, const UniformCache& uc,
                     uint64_t vsHash, uint32_t issued, uint32_t diagIndex,
                     const uint32_t* R = nullptr);

// Say what the two draw-index knobs above selected, WITH THE DENOMINATOR: how
// many draws they were offered and how many they matched, plus any index that
// falls outside the frame and any token that was not a number at all. Call
// once per reported frame. Without it, an index naming no draw prints exactly
// what a draw with nothing to show prints.
void ReportDrawSelections();

// GEARS_DRAW_FRAME_LIST=1: one line per issued draw -- its primitive, its
// shaders, the state that can zero it, where its geometry comes from and
// whether the mirror covers it, and how many float constants each shader got
// that are non-zero. Captured output identifies one scalar as the final colour
// intensity for most draws, so it is printed by name to distinguish intentional
// black output from missing inputs.
void ListDraw(const uint32_t* R, const FrameDrawItem& d, const FrameDrawInputs& in,
              const ShaderXlate& vsX, const ShaderXlate& psX,
              const UniformCache& uc, uint32_t issued);

} // namespace gears::draw
