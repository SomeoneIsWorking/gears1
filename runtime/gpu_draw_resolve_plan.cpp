// Working out the frame's resolve routing. gpu_draw_resolve_plan.h says why
// each of the two passes is shaped the way it is.

#include "gpu_draw_resolve_plan.h"

#include <algorithm>

#include <lucent/log.h>

namespace gears::draw
{

ResolvePlan PlanResolves(const FrameDrawInputs& in, RenderTargetCache& RT)
{
    ResolvePlan plan;
    // the destination set was both too large and unattributed to a source.
    // Pass one: which (base, format) surfaces the frame actually RENDERS into.
    // A resolve reprograms RB_COLOR_INFO to describe the source in whatever
    // format the copy reads it as -- measured on an Act 1 frame, EDRAM base
    // 0x2d0 is rendered as k_8_8_8_8 but resolved variously as
    // k_2_10_10_10_AS_10_10_10_10, k_2_10_10_10_FLOAT_AS_16_16_16_16, k_16_16
    // and k_2_10_10_10_FLOAT. That resolve-time format is a REINTERPRETATION of
    // the same EDRAM bytes, not a different surface, so a resolve's source is
    // looked up by BASE against the surfaces the frame renders, not by the
    // (base, format) pair its own registers happen to carry.
    for (const FrameDrawItem& d : in.draws)
    {
        const uint32_t* R = d.registers();
            if (!R)
                continue;
        const uint32_t mode = R[0x2208] & 0x7;
        if (mode != 4 /*kColorDepth*/ && mode != 5 /*kDepthOnly*/)
            continue;
        RT.formatsPerBase[R[0x2001] & 0xFFF].insert((R[0x2001] >> 16) & 0xF);
    }

    // Pass two: the frame's resolves, each given its destination's host image.
    // RB_COPY_DEST_BASE -> (destination texture base, row offset within it).
    // How many colour resolves this frame will DISPATCH. The descriptor pool is
    // sized from this, and it has to be counted here rather than from the
    // ResolveEvent list, which is not filled until the draw-preparation loop
    // that runs after -- sizing from an empty list gave 8 sets for 12
    // dispatches, so four of them silently reused a set that a later dispatch
    // then overwrote, and the resolves those sets belonged to wrote nothing.
    // Depth resolve destinations. We do not serve them yet, so a binding that
    // names one reads stale guest memory -- and which SHADER does that is the
    // evidence needed to settle how the packed depth is laid out across the
    // destination's four components (catalog #35).
    for (const FrameDrawItem& d : in.draws)
    {
        const uint32_t* R = d.registers();
            if (!R)
                continue;
        if ((R[0x2208] & 0x7) != 6 /*kCopy*/)
            continue;
        const uint32_t srcSelect = R[0x2318] & 0x7;
        if (srcSelect >= 4) // a DEPTH resolve
        {
            ++plan.fromDepth;
            const uint32_t dd = R[0x2319] & ~0xFFFu;
            if (!dd)
                continue;
            plan.depthDests.insert(dd);
            ResolveTarget* drt = nullptr;
            uint32_t drow = 0;
            if (!RT.GetResolveTarget(dd, 0xFFFFFFFFu, R[0x231A] & 0x3FFF,
                    (R[0x231A] >> 16) & 0x3FFF, (R[0x231B] >> 7) & 0x3F,
                    /*isDepth=*/true, drt, drow))
                continue;
            plan.routing[dd] = {drt->base, drow};
            plan.dests.insert(drt->base);
            ++plan.drawCount;
            continue;
        }
        const uint32_t destBase = R[0x2319] & ~0xFFFu;
        if (!destBase)
            continue;
        static const uint32_t kColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
        const uint32_t info = R[kColorInfo[srcSelect & 3]];
        const uint32_t srcBase = info & 0xFFF;
        if (!RT.formatsPerBase.count(srcBase))
        { ++plan.unmatched; continue; }
        ResolveTarget* rt = nullptr;
        uint32_t rowOffset = 0;
        if (!RT.GetResolveTarget(destBase, srcBase, R[0x231A] & 0x3FFF,
                (R[0x231A] >> 16) & 0x3FFF, (R[0x231B] >> 7) & 0x3F,
                /*isDepth=*/false, rt, rowOffset))
            continue;
        // Where this destination base writes: which texture, and how many rows
        // into it. A tile's base is the texture's base plus whole rows.
        plan.routing[destBase] = {rt->base, rowOffset};
        plan.dests.insert(rt->base);
        ++plan.drawCount;
    }

    return plan;
}

void ReportResolvePlan(const ResolvePlan& plan)
{
    if (plan.fromDepth || plan.unmatched)
        lucent::info("draw", "frame resolves not served: {} from depth (no host depth"
            " texture chain yet), {} from an EDRAM base this frame never rendered",
            plan.fromDepth, plan.unmatched);
    {
        lucent::Line rd;
        rd.add("frame: {} resolve destinations (from kCopy draws):",
               plan.dests.size());
        for (uint32_t b : plan.dests)
            rd.add(" {:#x}", b);
        rd.flush(lucent::Level::Info, "draw");
    }

}

} // namespace gears::draw
