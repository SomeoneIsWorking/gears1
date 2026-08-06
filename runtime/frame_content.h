#pragma once

#include <cstdint>

#include "gpu_draw.h"

// Does this frame contain a SKINNED CHARACTER?
//
// WHY THIS EXISTS. Catalog #77's open question is whether this renderer can
// light a character at all, and it cannot be answered because there is nothing
// to compare against: the only skinned character draw in any capture in the
// tree is bright.gfr's draw 460, and that is the one that renders black. Three
// attempts to capture a second one failed -- Gears' camera sits behind the
// player, so a scripted walk frequently has them out of frame or against a
// wall. Each attempt costs about four minutes and one in three ends in catalog
// #44's crash, so capturing by hand is an expensive coin flip.
//
// This turns the coin flip into a loop. The run keeps scanning frames and dumps
// the FIRST one that contains a character, instead of dumping a frame chosen by
// wall-clock timing and finding out afterwards.
//
// THE TEST IS THE MICROCODE, NOT THE CONSTANT VALUES. A UE3 skinned mesh
// transforms each vertex by bone matrices fetched from a palette of float
// constants indexed by the vertex's own blend indices, which compiles to a
// float-constant read through the address register (a0). Rigid geometry has no
// reason to index its constants dynamically. So "this vertex shader indexes its
// float constants dynamically" is a structural property of the shader, decided
// by Xenia's ucode analysis, and it does not depend on guessing which constant
// rows look like a matrix.
namespace gears
{

// What the scan saw. Every field is here so a NEGATIVE carries its denominator:
// "no character in this frame" and "I could not read any of this frame's
// shaders" print differently, which is the whole point.
struct SkinnedFrameCensus
{
    bool available = false;   // false: built without the Xenos translator, so
                              // NOTHING was examined -- not a negative
    uint32_t draws = 0;       // draws offered to the scan
    uint32_t withVertexShader = 0; // of those, ones carrying vertex microcode
    uint32_t analyzed = 0;    // of those, ones whose microcode analysed
    uint32_t unanalyzable = 0;// analysis failed: a blind spot, not a negative
    uint32_t skinnedShaders = 0;   // distinct VS hashes that index dynamically
    uint32_t skinnedDraws = 0;     // draws using one, at any size
    uint32_t passingDraws = 0;     // skinned AND at least minIndices indices
    uint32_t largestIndices = 0;         // largest mesh in the frame
    uint32_t largestSkinnedIndices = 0;  // largest skinned mesh in the frame
    uint32_t minIndices = 0;             // the threshold applied
    // The draw that made this frame pass, as an index into inputs.draws.
    // -1 when none did.
    int32_t passingDraw = -1;

    bool Passed() const { return passingDraws > 0; }
};

// Scans a frame's draws. `minIndices` rejects the small skinned meshes a frame
// carries whether or not a character is on screen; see the census tool for
// where the number comes from.
SkinnedFrameCensus ScanForSkinnedCharacter(const FrameDrawInputs& in,
                                           uint32_t minIndices);

// Logs the census as ONE line, positive or negative, always with the
// denominators and the blind spots. A caller must not decide whether to print:
// silence is what a broken scan and an empty frame both look like.
void ReportSkinnedFrameCensus(const SkinnedFrameCensus& c);

// The default threshold, in indices. Measured, not guessed -- see
// docs/knobs.md's GEARS_SKINNED_MIN_INDICES row and the census run recorded on
// catalog #77.
constexpr uint32_t kDefaultSkinnedMinIndices = 6000;

} // namespace gears
