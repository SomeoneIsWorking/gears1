// Decoding a kCopy draw. gpu_draw_resolve_decode.h says why a resolve is not a
// draw; this is what its registers mean.

#include "gpu_draw_resolve_decode.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include <lucent/config.h>
#include <lucent/log.h>

#include "gpu_draw_formats.h"
#include "gpu_draw_pixels.h"
#include "gpu_draw_xlate.h"

namespace gears::draw
{

void DeriveResolveRect(const uint32_t* R, const FrameDrawInputs& in,
                       uint32_t W, uint32_t H, RenderTargetCache& RT,
                       int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1)
{
    x0 = 0; y0 = 0; x1 = int32_t(W); y1 = int32_t(H);
            const uint32_t vf0 = R[0x4800], vf1 = R[0x4801];
    if ((vf0 & 3) == 3 && ((vf1 >> 2) & 0xFFFFFF) == 6)
    {
        const uint64_t addr = uint64_t((vf0 >> 2) << 2);
        if (addr + 6 * 4 <= in.guestWindowBytes)
        {
            // Most vertices carry a negative half-pixel offset,
            // which PA_SU_VTX_CNTL.pix_center says to reverse.
            const float halfPixel =
                (R[0x2302] & 1) == 0 /*kD3DZero*/ ? 0.5f : 0.0f;
            int32_t fx[3], fy[3];
            for (int k = 0; k < 3; ++k)
            {
                uint32_t rx, ry;
                std::memcpy(&rx, in.guestBase + addr + k * 8, 4);
                std::memcpy(&ry, in.guestBase + addr + k * 8 + 4, 4);
                rx = __builtin_bswap32(rx);
                ry = __builtin_bswap32(ry);
                float vx, vy;
                std::memcpy(&vx, &rx, 4);
                std::memcpy(&vy, &ry, 4);
                fx[k] = FloatToFixed16p8(vx + halfPixel);
                fy[k] = FloatToFixed16p8(vy + halfPixel);
            }
            // Top-left rule: include .5 on the near edge, exclude it
            // on the far one -- both are (v + 127) >> 8.
            x0 = (std::min({fx[0], fx[1], fx[2]}) + 127) >> 8;
            y0 = (std::min({fy[0], fy[1], fy[2]}) + 127) >> 8;
            x1 = (std::max({fx[0], fx[1], fx[2]}) + 127) >> 8;
            y1 = (std::max({fy[0], fy[1], fy[2]}) + 127) >> 8;
            // The window offset moves the rectangle exactly as it
            // moves geometry, which is what puts a predicated tile's
            // rectangle in its own part of the surface.
            if ((R[0x2205] >> 16) & 1 /*vtx_window_offset_enable*/)
            {
                const uint32_t wo = R[0x2080];
                auto sign15 = [](uint32_t v) {
                    v &= 0x7FFF;
                    return int32_t(v) - int32_t((v & 0x4000) << 1);
                };
                const int32_t wx = sign15(wo), wy = sign15(wo >> 16);
                x0 += wx; x1 += wx; y0 += wy; y1 += wy;
            }
            draw::GuestViewport gv;
            if (draw::DeriveViewport(R, gv))
            {
                const int32_t sr = int32_t(gv.scissorX + gv.scissorW);
                const int32_t sb = int32_t(gv.scissorY + gv.scissorH);
                x0 = std::clamp(x0, int32_t(gv.scissorX), sr);
                x1 = std::clamp(x1, int32_t(gv.scissorX), sr);
                y0 = std::clamp(y0, int32_t(gv.scissorY), sb);
                y1 = std::clamp(y1, int32_t(gv.scissorY), sb);
            }
            // D3D9's Resolve aligns the rectangle to 8.
            x0 &= ~int32_t(7); y0 &= ~int32_t(7);
            x1 = (x1 + 7) & ~int32_t(7);
            y1 = (y1 + 7) & ~int32_t(7);
        }
        else
        {
            ++RT.resolveNoRect;
        }
    }
    else
    {
        ++RT.resolveNoRect;
    }
}

void PrepareResolveDraw(const uint32_t* R, const FrameDrawItem& d,
                        const FrameDrawInputs& in, uint32_t W, uint32_t H,
                        const std::map<uint32_t, std::pair<uint32_t, uint32_t>>& resolveRouting,
                        RenderTargetCache& RT, FrameCensus& CN,
                        std::vector<PreparedDraw>& prepared)
{
    const uint32_t srcSelect = R[0x2318] & 0x7;
    const uint32_t destBase = R[0x2319] & ~0xFFFu;
    static const uint32_t kColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
    const uint32_t srcBase = srcSelect < 4
        ? (R[kColorInfo[srcSelect & 3]] & 0xFFF) : 0xFFFFFFFFu;
    // Does this copy also clear? RB_COPY_CONTROL bit 9 is
    // depth_clear_enable, and the value is RB_DEPTH_CLEAR, in the EDRAM
    // depth format. This is read for EVERY kCopy draw, including the
    // depth resolves whose copy we cannot serve, because the clear is
    // real even when the copy is not.
    const bool clearsDepthHere = ((R[0x2318] >> 9) & 1) != 0;
    float depthClearHere = 0.0f;
    uint32_t stencilClearHere = 0;
    if (clearsDepthHere)
    {
        stencilClearHere = R[0x231D] & 0xFF;
        const uint32_t d24 = R[0x231D] >> 8;
        depthClearHere = ((R[0x2002] >> 16) & 1) == 1 /*kD24FS8*/
            ? Depth20e4To32(d24) : DepthUnorm24To32(d24);
    }
    auto route = resolveRouting.find(destBase);
    if (RT.formatsPerBase.count(srcBase) && route != resolveRouting.end())
    {
        PreparedDraw pd{};
        pd.isResolve = true;
        // RB_SURFACE_INFO at the COPY. A resolve's source is an EDRAM surface
        // and its sample count is what says how the copy's pixel rectangle maps
        // onto that surface's samples -- the one piece of state that decides
        // whether a 640x360 4X fill covers the whole 1280x720 destination or a
        // quarter of it (catalog #91). It was absent from every resolve row.
        pd.surfaceInfo = R[0x2000];
        pd.resolveSampleSelect = (R[0x2318] >> 4) & 0x7;
        // The submission index, so the per-draw table can put this
        // resolve back between the draws it separates. Without it every
        // resolve row reads "draw 0" and the frame's pass boundaries are
        // unordered, which is the same as not having them.
        pd.diagIndex = uint32_t(&d - in.draws.data());
        pd.clearsDepth = clearsDepthHere;
        pd.depthClearValue = depthClearHere;
        pd.stencilClearValue = stencilClearHere;
        pd.surfaceBase = srcBase;
        pd.resolveDestFormat = (R[0x231B] >> 7) & 0x3F;
        // What the copy reads the source under. Indexed by copy_src_select,
        // like srcBase just above it -- reading RT0's format here instead is
        // what hid every resolve's real format behind a uniform 0.
        pd.resolveSrcFormat = srcSelect < 4
            ? ((R[kColorInfo[srcSelect & 3]] >> 16) & 0xF) : 0u;
        pd.resolveDest = route->second.first;   // the TEXTURE, not the base
        // The resolve rectangle, per Xenia's GetResolveInfo: three
        // vertices of two floats in vertex fetch constant 0 ("D3D9 HACK:
        // Vertices to use are always in vf0, and are written by the
        // CPU"), converted to 16p8 fixed point, min/max'd, rounded by
        // the top-left rule, shifted by the window offset and clamped to
        // the scissor.
        int32_t x0 = 0, y0 = 0, x1 = int32_t(W), y1 = int32_t(H);
        DeriveResolveRect(R, in, W, H, RT, x0, y0, x1, y1);
        pd.resolveSrcRect.offset = {x0, y0};
        pd.resolveSrcRect.extent = {uint32_t(std::max(0, x1 - x0)),
                                    uint32_t(std::max(0, y1 - y0))};
        // The destination offset is the rectangle's own origin plus the
        // rows this base sits into the texture.
        pd.resolveDstX = x0;
        pd.resolveDstY = y0 + int32_t(route->second.second);
        // The guest's own copy state: a signed exponent bias applied to
        // the colour on its way out of EDRAM, and the red/blue swap.
        {
            const uint32_t rawBias = (R[0x231B] >> 16) & 0x3F;
            const int32_t bias = int32_t(rawBias) - int32_t((rawBias & 0x20) << 1);
            pd.resolveScale = std::ldexp(1.0f, bias);
            // GEARS_DRAW_RESOLVE_SCALE=<float> forces the factor. A
            // DIAGNOSTIC control arm: at 1.0 the compute resolve must
            // reproduce the old blit exactly, which is how the compute
            // path is separated from the bias value it applies.
            // BY VALUE -- see the note in gpu_draw_shaders.cpp: a reference
            // into lucent's config cache dangles when the cache is dropped.
            static const std::string forced{
                lucent::config::text("DRAW_RESOLVE_SCALE")};
            if (!forced.empty())
                pd.resolveScale = float(std::atof(forced.c_str()));
            pd.resolveSwapRB = ((R[0x231B] >> 24) & 1) != 0;
            // GEARS_DRAW_RESOLVE_NOSWAP=1 suppresses the red/blue swap.
            // A DIAGNOSTIC control arm: the blit path cannot swap, so
            // the compute path only has to match it byte-for-byte with
            // the swap disabled and the scale forced to 1.
            static const bool noSwap =
                lucent::config::flag("DRAW_RESOLVE_NOSWAP");
            if (noSwap)
                pd.resolveSwapRB = false;
        }
        prepared.push_back(pd);
        ++CN.issuedResolves;
    }
    else
    {
        ++CN.skippedResolves;
        // The copy cannot be served -- a depth resolve, with no host
        // depth texture chain yet -- but its CLEAR still has to happen,
        // at this point in the stream and with the guest's value.
        // A DEPTH resolve now has a destination of its own, so it is
        // emitted rather than dropped -- with its clear, if it carries
        // one, still happening after it.
        auto droute = resolveRouting.find(destBase);
        const bool isDepthResolve = srcSelect >= 4 &&
                                    droute != resolveRouting.end();
        if (isDepthResolve || clearsDepthHere)
        {
            PreparedDraw pd{};
            pd.isResolve = true;
            pd.surfaceInfo = R[0x2000];
            pd.resolveSampleSelect = (R[0x2318] >> 4) & 0x7;
            pd.diagIndex = uint32_t(&d - in.draws.data());
            pd.copyIsServed = false;
            pd.clearsDepth = clearsDepthHere;
            pd.depthClearValue = depthClearHere;
        pd.stencilClearValue = stencilClearHere;
            if (isDepthResolve)
            {
                pd.resolveIsDepth = true;
                pd.resolveDepthIsFloat24 = ((R[0x2002] >> 16) & 1) == 1;
                // A depth copy's own identity: the depth surface it reads, and
                // the format RB_DEPTH_INFO implies rather than the one
                // RB_COPY_DEST_INFO carries.
                pd.surfaceBase = R[0x2002] & 0xFFF;
                pd.resolveDestFormat = DepthDestFormat(R[0x2002]);
                pd.resolveDest = droute->second.first;
                int32_t x0 = 0, y0 = 0, x1 = int32_t(W), y1 = int32_t(H);
                DeriveResolveRect(R, in, W, H, RT, x0, y0, x1, y1);
                pd.resolveSrcRect.offset = {x0, y0};
                pd.resolveSrcRect.extent = {uint32_t(std::max(0, x1 - x0)),
                                            uint32_t(std::max(0, y1 - y0))};
                pd.resolveDstX = x0;
                pd.resolveDstY = y0 + int32_t(droute->second.second);
            }
            if (clearsDepthHere)
                ++RT.midFrameDepthClears;
            prepared.push_back(pd);
        }
    }
}

} // namespace gears::draw
