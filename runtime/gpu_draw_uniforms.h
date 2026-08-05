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

private:
    bool valid = false;
    const void* keySnapshot = nullptr;
    uint64_t keyVs = 0, keyPs = 0;
};

} // namespace gears::draw
