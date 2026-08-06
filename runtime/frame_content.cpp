// See frame_content.h for why this exists.
#include "frame_content.h"

#include <lucent/log.h>

#include <unordered_map>
#include <unordered_set>

#ifdef GEARS_HAVE_GUEST_DRAW
#include "gpu_draw_xlate.h"
#endif

namespace gears
{

SkinnedFrameCensus ScanForSkinnedCharacter(const FrameDrawInputs& in,
                                           uint32_t minIndices)
{
    SkinnedFrameCensus c;
    c.minIndices = minIndices;
    c.draws = uint32_t(in.draws.size());
#ifndef GEARS_HAVE_GUEST_DRAW
    // available stays false. A build with no translator examined nothing, and
    // reporting that as "no character" would certify a frame nobody looked at.
    return c;
#else
    c.available = true;
    // One verdict per distinct vertex shader. A frame binds a few dozen across
    // hundreds of draws, and the analysis behind this is itself cached by hash,
    // so a whole frame costs a handful of analyses.
    std::unordered_map<uint64_t, draw::VertexShaderShape> byHash;
    std::unordered_set<uint64_t> skinnedHashes;
    for (uint32_t i = 0; i < c.draws; ++i)
    {
        const FrameDrawItem& d = in.draws[i];
        c.largestIndices = std::max(c.largestIndices, d.indexCount);
        if (!d.vsUcode || d.vsUcodeSize == 0)
            continue;
        ++c.withVertexShader;
        auto it = byHash.find(d.vsHash);
        if (it == byHash.end())
            it = byHash.emplace(d.vsHash,
                draw::AnalyzeVertexShaderShape(d.vsUcode, d.vsUcodeSize,
                                               d.vsHash)).first;
        const draw::VertexShaderShape& s = it->second;
        if (!s.ok)
        {
            ++c.unanalyzable;
            continue;
        }
        ++c.analyzed;
        if (!s.floatDynamicAddressing)
            continue;
        ++c.skinnedDraws;
        skinnedHashes.insert(d.vsHash);
        c.largestSkinnedIndices = std::max(c.largestSkinnedIndices, d.indexCount);
        if (d.indexCount < minIndices)
            continue;
        ++c.passingDraws;
        // Keep the BIGGEST passing draw, not the first: a frame that contains a
        // character also contains their weapon and their gear, and the mesh
        // worth pointing a later investigation at is the largest one.
        if (c.passingDraw < 0 ||
            d.indexCount > in.draws[size_t(c.passingDraw)].indexCount)
            c.passingDraw = int32_t(i);
    }
    c.skinnedShaders = uint32_t(skinnedHashes.size());
    return c;
#endif
}

void ReportSkinnedFrameCensus(const SkinnedFrameCensus& c)
{
    if (!c.available)
    {
        lucent::warn("draw", "skinned-character scan: UNAVAILABLE -- this build"
            " has no Xenos translator, so NO draw was examined. This is not a"
            " negative result");
        return;
    }
    if (c.Passed())
    {
        lucent::info("draw", "skinned-character scan: FOUND -- {} of {} draws use"
            " a vertex shader that indexes float constants dynamically (a bone"
            " palette), {} of them at or above the {}-index threshold; largest"
            " skinned mesh {} indices, largest mesh in the frame {}; {} distinct"
            " skinned shaders, {} draws could not be analysed",
            c.skinnedDraws, c.draws, c.passingDraws, c.minIndices,
            c.largestSkinnedIndices, c.largestIndices, c.skinnedShaders,
            c.unanalyzable);
        return;
    }
    // THE NEGATIVE, written before the positive was: it has to say what was
    // scanned, what the frame's biggest mesh actually was, and what this test
    // cannot see -- otherwise "no character" is indistinguishable from a scan
    // that never ran.
    lucent::info("draw", "skinned-character scan: NONE -- scanned {} draws ({}"
        " carried vertex microcode, {} analysed, {} could NOT be analysed and are"
        " a blind spot); {} draws used a dynamically-indexed constant palette,"
        " largest of them {} indices, none reached the {}-index threshold;"
        " largest mesh in the frame {} indices. Blind to a character drawn"
        " WITHOUT a GPU bone palette (CPU-skinned or morph-target geometry)",
        c.draws, c.withVertexShader, c.analyzed, c.unanalyzable, c.skinnedDraws,
        c.largestSkinnedIndices, c.minIndices, c.largestIndices);
}

} // namespace gears
