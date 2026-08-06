#pragma once

// The five per-draw constant blocks -- system constants, the vertex and pixel
// float constants, the bool/loop registers and the fetch constants -- and the
// cache that stops them being rebuilt for every draw.
//
// They derive ONLY from the register snapshot and the two shaders' constant
// bitmaps, so consecutive draws sharing a snapshot and a shader pair produce
// BYTE-IDENTICAL blocks. Recomputing them per draw meant five heap allocations
// and five arena copies each -- about 3500 allocations in a gameplay frame --
// for data that had not changed. Correctness comes from the key being the whole
// input, not from a heuristic.
//
// The census is not optional. This cache exists to remove what was 118 ms of a
// 187 ms frame, and the comparison is by POINTER on the register snapshot, so a
// draw carrying its own copy would never match however identical the contents.
// A hit rate nobody prints is a cache nobody can tell is working.

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "gpu_draw_arena.h"
#include "gpu_draw_xlate.h"

namespace gears::draw
{

struct UniformCache
{
    explicit UniformCache(FrameArena& arena) : AR(arena) {}

    FrameArena& AR;

    enum class Result { kReused, kRebuilt, kFailed };

    // Returns kReused when this draw's blocks are already the cached ones,
    // kRebuilt when they were repacked, and kFailed when the arena could not
    // hold them -- on which the caller must SKIP the draw, because the
    // descriptor infos then describe the previous draw's constants.
    Result Update(const uint32_t* regs, const FrameDrawItem& d,
                  const ShaderXlate& vsX, const ShaderXlate& psX);

    // The packed bytes, kept after the call so a diagnostic can print the
    // numbers the shader will actually multiply by (GEARS_DRAW_PS_CONSTS).
    std::vector<uint8_t> sysc, fVs, fPs, boolLoop, fetch;

    // Where each block landed in the arena. Valid unless Update returned
    // kFailed.
    VkDescriptorBufferInfo biSys{}, biFvs{}, biFps{}, biBl{}, biFetch{};

    uint64_t lookups = 0, hits = 0, missSnapshot = 0, missShaders = 0;
    uint64_t rebuilds = 0, reuses = 0;
    // GEARS_DRAW_UBOCHECK=1 only: how many repacks came out byte-identical to
    // the blocks already cached -- exactly the work a content comparison would
    // let us skip, and the number that says whether narrowing the key is worth
    // anything. It costs an extra copy and compare per miss, so it is gated.
    uint64_t recomputes = 0, recomputesIdentical = 0;

    // ---- the NaN/Inf census over the constants a shader actually receives ----
    //
    // A NaN in one pixel constant took a whole gameplay frame to 0 of 921,600
    // non-black pixels, and it took two sessions to find because nothing in the
    // renderer looks at the VALUES it packs -- the frame reported "0 px
    // non-black" and every instrument pointed at the draws, which were all
    // fine (`catalog.py show 73`). A frame that is black because it was handed
    // a NaN must SAY so, in a normal run, without anyone having guessed the
    // shader hash first.
    //
    // Cheap enough to leave always on: it scans only on a cache REBUILD, and
    // only the packed float block, which is 10-16 vec4s.
    struct BadConst
    {
        uint64_t psHash;
        uint32_t index;      // vec4 index in the shader's own packed order
        uint32_t bits[4];
        bool isPixel;
    };
    uint64_t nanBlocks = 0, infBlocks = 0;
    std::vector<BadConst> badConsts;     // capped; nanBlocks is the denominator
    static constexpr size_t kMaxBadConsts = 8;
    // Scans one packed block and records what it finds. `isPixel` only labels
    // the report.
    void CensusConstants(const std::vector<uint8_t>& block, uint64_t psHash,
                         bool isPixel);
    // One line per frame, or SILENCE ONLY when a scan actually ran and found
    // nothing -- which it says, with its denominator.
    void ReportConstantCensus() const;

private:
    bool valid = false;
    const void* keySnapshot = nullptr;
    uint64_t keyVs = 0, keyPs = 0;
};

} // namespace gears::draw
