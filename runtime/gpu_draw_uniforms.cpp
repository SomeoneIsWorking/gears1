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

UniformCache::Result UniformCache::Update(const uint32_t* regs, const FrameDrawItem& d,
                                          const ShaderXlate& vsX, const ShaderXlate& psX)
{
    const bool same = valid && keySnapshot == d.registerFile.get() &&
                      keyVs == d.vsHash && keyPs == d.psHash;
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
    const bool measureHeadroom = lucent::config::flag("DRAW_UBOCHECK");
    std::vector<uint8_t> prevSysc, prevFvs, prevFps, prevBl, prevFetch;
    if (measureHeadroom && valid)
    {
        prevSysc = sysc; prevFvs = fVs; prevFps = fPs;
        prevBl = boolLoop; prevFetch = fetch;
    }

    sysc = DeriveSystemConstants(regs);
    fVs = PackFloatConstants(regs, vsX.floatBitmap, vsX.floatCount, 0x4000);
    fPs = PackFloatConstants(regs, psX.floatBitmap, psX.floatCount, 0x4400);
    // CONTROL ARM. GEARS_DRAW_PS_CONST_SET=<pshash>:<i>=<x>,<y>,<z>,<w>
    // (';'-separated for several) replaces one packed float constant of one
    // pixel shader. It answers exactly one question -- "is the picture wrong
    // because of THIS number?" -- by substituting the value a working capture
    // has. It is never a fix: the number comes from the guest, and a wrong one
    // is a bug on the CPU side, not here.
    if (const std::string& setSpec = lucent::config::text("DRAW_PS_CONST_SET");
        !setSpec.empty())
    {
        size_t at = 0;
        while (at < setSpec.size())
        {
            const size_t end = std::min(setSpec.find(';', at), setSpec.size());
            const std::string one = setSpec.substr(at, end - at);
            at = end + 1;
            const size_t colon = one.find(':'), eq = one.find('=');
            if (colon == std::string::npos || eq == std::string::npos || eq < colon)
            { lucent::warn("draw", "GEARS_DRAW_PS_CONST_SET: cannot parse"
                " '{}', expected <pshash>:<index>=<x>,<y>,<z>,<w>", one); continue; }
            const uint64_t h = std::strtoull(one.c_str(), nullptr, 16);
            if (h != d.psHash)
                continue;
            const uint32_t idx = uint32_t(std::strtoul(
                one.c_str() + colon + 1, nullptr, 10));
            float v[4] = {0, 0, 0, 0};
            const char* p = one.c_str() + eq + 1;
            for (float& f : v)
            { char* nxt = nullptr; f = std::strtof(p, &nxt);
              if (nxt == p) break; p = (*nxt == ',') ? nxt + 1 : nxt; }
            if ((idx + 1) * 16 > fPs.size())
            { lucent::warn("draw", "GEARS_DRAW_PS_CONST_SET: ps {:#x} has"
                " {} packed constants, so index {} DOES NOT EXIST and was"
                " NOT applied", d.psHash, psX.floatCount, idx); continue; }
            std::memcpy(fPs.data() + size_t(idx) * 16, v, 16);
            lucent::info("draw", "GEARS_DRAW_PS_CONST_SET: ps {:#x} c[{}]"
                " forced to ({}, {}, {}, {})", d.psHash, idx,
                v[0], v[1], v[2], v[3]);
        }
    }
    boolLoop.resize(sizeof(uint32_t) * (8 + 32));
    std::memcpy(boolLoop.data(), &regs[0x4900], boolLoop.size());
    fetch.resize(sizeof(uint32_t) * 6 * 32);
    std::memcpy(fetch.data(), &regs[0x4800], fetch.size());

    if (!AR.MakeUbo(sysc.data(), sysc.size(), biSys) ||
        !AR.MakeUbo(fVs.data(), fVs.size(), biFvs) ||
        !AR.MakeUbo(fPs.data(), fPs.size(), biFps) ||
        !AR.MakeUbo(boolLoop.data(), boolLoop.size(), biBl) ||
        !AR.MakeUbo(fetch.data(), fetch.size(), biFetch))
    {
        valid = false;
        return Result::kFailed;
    }

    if (measureHeadroom && !prevSysc.empty())
    {
        ++recomputes;
        if (prevSysc == sysc && prevFvs == fVs && prevFps == fPs &&
            prevBl == boolLoop && prevFetch == fetch)
            ++recomputesIdentical;
    }

    valid = true;
    keySnapshot = d.registerFile.get();
    keyVs = d.vsHash;
    keyPs = d.psHash;
    ++rebuilds;
    return Result::kRebuilt;
}

} // namespace gears::draw
