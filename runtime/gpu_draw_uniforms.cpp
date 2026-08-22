// The per-draw constant blocks and their cache. gpu_draw_uniforms.h says what
// the key is and why; this is the pack.

#include "gpu_draw_uniforms.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include <lucent/config.h>
#include <lucent/log.h>

#include "gpu_draw_pixels.h"

namespace gears::draw
{

UniformCache::Result UniformCache::Update(const uint32_t *regs, const FrameDrawItem &d,
                                          const ShaderXlate &vsX, const ShaderXlate &psX)
{
    const bool same =
        valid && keySnapshot == d.registerFile.get() && keyVs == d.vsHash && keyPs == d.psHash;
    ++lookups;
    if (same)
    {
        ++hits;
        ++reuses;
        return Result::kReused;
    }
    if (valid)
    {
        // Which part of the key differed. A snapshot-pointer miss with equal
        // shader hashes is the shape that would mean the cache can never work.
        if (keySnapshot != d.registerFile.get())
            ++missSnapshot;
        else
            ++missShaders;
    }

    // MEASURING THE HEADROOM. Every uniform-cache miss is on the register
    // snapshot POINTER -- 114 of 114 in a measured frame, none on the shader
    // pair -- and the snapshot is already shared between draws that changed no
    // register, so those misses mean the guest really did write registers.
    //
    // But a register write does not imply the UNIFORM blocks changed: the guest
    // may have touched a viewport or a state register that feeds none of them.
    // This counts how often the repacked blocks come out byte-identical to the
    // ones already cached. Without it, narrowing the cache key would be a guess
    // at where the time goes.
    static const bool measureHeadroom = lucent::config::flag("DRAW_UBOCHECK");
    std::vector<uint8_t> prevSysc, prevFvs, prevFps, prevBl, prevFetch;
    if (measureHeadroom && valid)
    {
        prevSysc = sysc;
        prevFvs = fVs;
        prevFps = fPs;
        prevBl = boolLoop;
        prevFetch = fetch;
    }

    DeriveSystemConstants(regs, sysc);
    PackFloatConstants(regs, vsX.floatBitmap, vsX.floatCount, 0x4000, fVs);
    PackFloatConstants(regs, psX.floatBitmap, psX.floatCount, 0x4400, fPs);
    // Look at the VALUES, not just pack them. See gpu_draw_uniforms.h.
    CensusConstants(fPs, d.psHash, true);
    CensusConstants(fVs, d.vsHash, false);
    // CONTROL ARM. GEARS_DRAW_PS_CONST_SET=<pshash>:<i>=<x>,<y>,<z>,<w>
    // (';'-separated for several) replaces one packed float constant of one
    // pixel shader. It answers exactly one question -- "is the picture wrong
    // because of THIS number?" -- by substituting the value a working capture
    // has. It is never a fix: the number comes from the guest, and a wrong one
    // is a bug on the CPU side, not here.
    static const std::string &setSpec = lucent::config::text("DRAW_PS_CONST_SET");
    if (!setSpec.empty())
    {
        size_t at = 0;
        while (at < setSpec.size())
        {
            const size_t end = std::min(setSpec.find(';', at), setSpec.size());
            const std::string one = setSpec.substr(at, end - at);
            at = end + 1;
            const size_t colon = one.find(':'), eq = one.find('=');
            if (colon == std::string::npos || eq == std::string::npos || eq < colon)
            {
                lucent::warn("draw",
                             "GEARS_DRAW_PS_CONST_SET: cannot parse"
                             " '{}', expected <pshash>:<index>=<x>,<y>,<z>,<w>",
                             one);
                continue;
            }
            const uint64_t h = std::strtoull(one.c_str(), nullptr, 16);
            if (h != d.psHash)
                continue;
            const uint32_t idx = uint32_t(std::strtoul(one.c_str() + colon + 1, nullptr, 10));
            float v[4] = {0, 0, 0, 0};
            const char *p = one.c_str() + eq + 1;
            for (float &f : v)
            {
                char *nxt = nullptr;
                f = std::strtof(p, &nxt);
                if (nxt == p)
                    break;
                p = (*nxt == ',') ? nxt + 1 : nxt;
            }
            if ((idx + 1) * 16 > fPs.size())
            {
                lucent::warn("draw",
                             "GEARS_DRAW_PS_CONST_SET: ps {:#x} has"
                             " {} packed constants, so index {} DOES NOT EXIST and was"
                             " NOT applied",
                             d.psHash, psX.floatCount, idx);
                continue;
            }
            std::memcpy(fPs.data() + size_t(idx) * 16, v, 16);
            lucent::info("draw",
                         "GEARS_DRAW_PS_CONST_SET: ps {:#x} c[{}]"
                         " forced to ({}, {}, {}, {})",
                         d.psHash, idx, v[0], v[1], v[2], v[3]);
        }
    }
    boolLoop.resize(sizeof(uint32_t) * (8 + 32));
    std::memcpy(boolLoop.data(), &regs[0x4900], boolLoop.size());
    fetch.resize(sizeof(uint32_t) * 6 * 32);
    std::memcpy(fetch.data(), &regs[0x4800], fetch.size());

    if (!AR.MakeUbo(sysc.data(), sysc.size(), biSys) ||
        !AR.MakeUbo(fVs.data(), fVs.size(), biFvs) || !AR.MakeUbo(fPs.data(), fPs.size(), biFps) ||
        !AR.MakeUbo(boolLoop.data(), boolLoop.size(), biBl) ||
        !AR.MakeUbo(fetch.data(), fetch.size(), biFetch))
    {
        valid = false;
        return Result::kFailed;
    }

    if (measureHeadroom && !prevSysc.empty())
    {
        ++recomputes;
        if (prevSysc == sysc && prevFvs == fVs && prevFps == fPs && prevBl == boolLoop &&
            prevFetch == fetch)
            ++recomputesIdentical;
    }

    valid = true;
    keySnapshot = d.registerFile.get();
    keyVs = d.vsHash;
    keyPs = d.psHash;
    ++rebuilds;
    return Result::kRebuilt;
}

// A NaN or an Inf in a constant a shader will multiply by. See the header for
// why this is not gated behind a knob.
void UniformCache::CensusConstants(const std::vector<uint8_t> &block, uint64_t hash, bool isPixel)
{
    for (size_t i = 0; i + 16 <= block.size(); i += 16)
    {
        uint32_t b[4];
        std::memcpy(b, block.data() + i, 16);
        bool nan = false, inf = false;
        for (int k = 0; k < 4; ++k)
        {
            const uint32_t exponent = (b[k] >> 23) & 0xFFu;
            const uint32_t mantissa = b[k] & 0x7FFFFFu;
            if (exponent != 0xFFu)
                continue;
            if (mantissa)
                nan = true;
            else
                inf = true;
        }
        if (!nan && !inf)
            continue;
        if (nan)
            ++nanBlocks;
        else
            ++infBlocks;
        // NaN is the one that kills a frame outright, so it wins the capped
        // slots: an Inf is a normal value of at least one of this title's
        // constant blocks (catalog #73 killed that hypothesis by running it
        // against both classes) and must not crowd out a real finding.
        if (nan && badConsts.size() < kMaxBadConsts)
            badConsts.push_back(
                BadConst{hash, uint32_t(i / 16), {b[0], b[1], b[2], b[3]}, isPixel});
    }
}

void UniformCache::ReportConstantCensus() const
{
    if (nanBlocks == 0)
    {
        // THE NEGATIVE, with its denominator. "No NaN constants" and "nothing
        // scanned" must not look the same, and this scan runs only on a cache
        // REBUILD -- so the denominator is rebuilds, not draws.
        if (rebuilds != 0)
            lucent::debug("draw",
                          "constant census: no NaN in any float"
                          " constant, over {} repacked block set(s) ({} carried an Inf,"
                          " which is a normal value of this title's c1)",
                          rebuilds, infBlocks);
        return;
    }
    lucent::Line l;
    l.add("CONSTANT CENSUS: {} packed constant vec4(s) contain a NaN, over {}"
          " repacked block set(s). A shader multiplying by one of these writes"
          " NaN, and a NaN written to a colour target reads back as BLACK --"
          " so a frame that renders black for this reason is NOT a renderer"
          " defect and no draw-level probe will show it (catalog #73).",
          nanBlocks, rebuilds);
    for (const BadConst &c : badConsts)
        l.add("\n  {} shader {:#x}  c[{}] = [{:08x} {:08x} {:08x} {:08x}]",
              c.isPixel ? "pixel " : "vertex", c.psHash, c.index, c.bits[0], c.bits[1], c.bits[2],
              c.bits[3]);
    if (nanBlocks > badConsts.size())
        l.add("\n  ... and {} further NaN vec4(s) not listed (cap {}); the"
              " count above is the whole frame, the list is not",
              nanBlocks - badConsts.size(), kMaxBadConsts);
    l.add("\n  Raw bits matter: ffc00000 is x86's default QNaN (host"
          " arithmetic made it), 7fc00000 is PowerPC's, ffffffff is"
          " uninitialised memory. They point at different bugs.");
    l.flush(lucent::Level::Warn, "draw");
}

} // namespace gears::draw
