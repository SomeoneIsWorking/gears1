// What a draw fetches. gpu_draw_vertexfetch.h says why the range matters twice
// over -- as the upload list and as the reach census.

#include "gpu_draw_vertexfetch.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <string>

#include <lucent/config.h>
#include <lucent/log.h>

#include "gpu_draw_formats.h"

namespace gears::draw
{

void CollectFetchRanges(const uint32_t* R, const FrameDrawInputs& in,
                        const ShaderXlate& vsX, const ShaderXlate& psX,
                        FrameCensus& CN,
                        std::vector<std::pair<uint64_t, uint64_t>>& fetchRanges)
{
    bool anyPast = false, anyBinding = false;
    for (const auto& vb : vsX.vertexBindings)
    {
        const uint32_t fc = vb.fetchConstant & 95;
        const uint32_t d0 = R[0x4800 + fc * 2];
        const uint32_t d1 = R[0x4800 + fc * 2 + 1];
        if ((d0 & 3) != 3 /*kVertex*/)
            continue;
        anyBinding = true;
        const uint64_t begin = uint64_t((d0 >> 2) << 2);
        const uint64_t end = begin + ((d1 >> 2) & 0xFFFFFF) * 4ull;
        CN.vfHighestByte = std::max<uint32_t>(CN.vfHighestByte, uint32_t(std::min<uint64_t>(end, 0xFFFFFFFFull)));
        if (end > in.guestPhysicalMirrorBytes)
            anyPast = true;
        // This is the range the GPU will fetch from, so it is also
        // exactly the range that has to be uploaded.
        else if (end > begin)
            fetchRanges.emplace_back(begin, end);
    }
    if (anyBinding)
        (anyPast ? CN.vfDrawsPastMirror : CN.vfDrawsInMirror) += 1;
    // A pixel shader may carry vertex fetches of its own. They are not
    // part of the geometry-reach census (which is about whether this
    // draw's GEOMETRY resolves), but they are still memory the GPU will
    // read, so they still have to be uploaded.
    for (const auto& vb : psX.vertexBindings)
    {
        const uint32_t fc = vb.fetchConstant & 95;
        const uint32_t d0 = R[0x4800 + fc * 2];
        const uint32_t d1 = R[0x4800 + fc * 2 + 1];
        if ((d0 & 3) != 3 /*kVertex*/)
            continue;
        const uint64_t begin = uint64_t((d0 >> 2) << 2);
        const uint64_t end = begin + ((d1 >> 2) & 0xFFFFFF) * 4ull;
        if (end > begin && end <= in.guestPhysicalMirrorBytes)
            fetchRanges.emplace_back(begin, end);
    }
}


void DumpVertices(const uint32_t* R, const FrameDrawInputs& in,
                  const ShaderXlate& vsX, uint32_t issued, uint32_t diagIndex)
{
    static const long vdump = lucent::config::number("DRAW_VDUMP", -1);
    if (vdump >= 0 && long(diagIndex) == vdump)
    {
        for (const auto& vb : vsX.vertexBindings)
        {
            const uint32_t fc = vb.fetchConstant & 95;
            const uint32_t d0 = R[0x4800 + fc * 2];
            if ((d0 & 3) != 3) continue;
            const uint32_t vbase = (d0 >> 2) << 2;
            const uint32_t stride = std::max(vb.strideWords, 1u);
            for (uint32_t v = 0; v < 4; ++v)
            {
                lucent::Line vl;
                vl.add("  draw {} vertex {} @ {:#x} (stride {} dwords):",
                       issued, v, vbase + v * stride * 4, stride);
                for (uint32_t w = 0; w < stride; ++w)
                {
                    const uint64_t off = uint64_t(vbase) + (v * stride + w) * 4;
                    if (off + 4 > in.guestPhysicalMirrorBytes) break;
                    uint32_t raw;
                    std::memcpy(&raw, in.guestBase + off, 4);
                    raw = __builtin_bswap32(raw);
                    float f;
                    std::memcpy(&f, &raw, 4);
                    vl.add(" [{}]{:#010x}={}", w, raw, f);
                }
                vl.flush(lucent::Level::Info, "draw");
            }
        }
    }
}

void ListDraw(const uint32_t* R, const FrameDrawItem& d, const FrameDrawInputs& in,
              const ShaderXlate& vsX, const ShaderXlate& psX,
              const UniformCache& uc, uint32_t issued)
{
    lucent::Line dl;
    dl.add("  draw {}: {} {} {} verts, vs {:#018x} ({} tex) ps {:#018x} ({} tex),"
           " colormask {:#x} blend {:#x} depth {:#x}",
           issued, PrimName(d.primType), d.indexed ? "indexed" : "auto",
           d.indexCount, d.vsHash, vsX.textures.size(), d.psHash,
           psX.textures.size(), R[0x2104] /*RB_COLOR_MASK*/,
           R[0x2201] /*RB_BLENDCONTROL0*/, R[0x2200] /*RB_DEPTHCONTROL*/);
    for (const auto& t : psX.textures)
        dl.add(" tex[fc{}]={:#x}", t.fetchConstant,
               (R[0x4800 + (t.fetchConstant & 31) * 6 + 1] >> 12) << 12);
    // Where this draw's GEOMETRY comes from, and whether the SSBO
    // mirror actually covers it. A vfetch past the mirror reads zero,
    // which collapses every triangle -- indistinguishable in the output
    // from "shaded black", so it has to be reported explicitly.
    for (const auto& vb : vsX.vertexBindings)
    {
        const uint32_t fc = vb.fetchConstant & 95;
        const uint32_t d0 = R[0x4800 + fc * 2];
        const uint32_t d1 = R[0x4800 + fc * 2 + 1];
        const uint32_t vbase = (d0 >> 2) << 2;          // dword address -> bytes
        const uint32_t vbytes = ((d1 >> 2) & 0xFFFFFF) * 4; // size in dwords -> bytes
        dl.add(" vf[fc{}]type{}={:#x}+{:#x}{}", fc, d0 & 3, vbase, vbytes,
               uint64_t(vbase) + vbytes <= in.guestPhysicalMirrorBytes
                   ? "" : " PAST-MIRROR");
    }
    // The float constants the shaders actually got. Nearly every pixel
    // shader in this frame ends in `mul oC0.xyz, r, c255.x`, so a zero
    // c255 makes the draw black no matter what it sampled.
    auto nonZero = [](const std::vector<uint8_t>& v) {
        size_t n = 0;
        for (size_t i = 0; i + 4 <= v.size(); i += 4)
            if (v[i] || v[i + 1] || v[i + 2] || v[i + 3]) ++n;
        return n;
    };
    float psC255 = 0.0f;
    const uint32_t c255bits = R[0x4400 + 255 * 4];
    std::memcpy(&psC255, &c255bits, 4);
    dl.add(" vsconst {}/{} nz, psconst {}/{} nz, ps c255.x={} ({:#x})",
           nonZero(uc.fVs), vsX.floatCount, nonZero(uc.fPs), psX.floatCount,
           psC255, c255bits);
    // GEARS_DRAW_PS_CONSTS=<hash> prints the pixel float constants a named
    // shader actually received, as the numbers the shader will multiply by.
    // "psconst 9/3 nz" says three of nine are non-zero and cannot say WHICH,
    // and for a pass that ends in a scale the difference between "the scale
    // is 0.9" and "the scale is 0" is the difference between a frame and a
    // black screen.
    if (const std::string& want = lucent::config::text("DRAW_PS_CONSTS");
        !want.empty())
    {
        const uint64_t wantHash = std::strtoull(want.c_str(), nullptr, 16);
        if (wantHash == d.psHash)
        {
            lucent::Line cl;
            cl.add("draw {} ps {:#x} float constants ({} vec4s, in the"
                   " shader's own packed order):", issued, d.psHash,
                   psX.floatCount);
            for (uint32_t i = 0; i < psX.floatCount &&
                                 (i + 1) * 16 <= uc.fPs.size(); ++i)
            {
                float v[4]; uint32_t b[4];
                std::memcpy(v, uc.fPs.data() + size_t(i) * 16, 16);
                std::memcpy(b, uc.fPs.data() + size_t(i) * 16, 16);
                // The RAW BITS as well, because a NaN or an inf printed as
                // a word says nothing about where it came from: 0xffffffff
                // is uninitialised memory, 0x7fc00000 is arithmetic, and the
                // two point at completely different bugs.
                cl.add(" c[{}]=({}, {}, {}, {})[{:08x} {:08x} {:08x} {:08x}]",
                       i, v[0], v[1], v[2], v[3], b[0], b[1], b[2], b[3]);
            }
            cl.flush(lucent::Level::Info, "draw");
        }
    }
    dl.flush(lucent::Level::Info, "draw");
}

} // namespace gears::draw
