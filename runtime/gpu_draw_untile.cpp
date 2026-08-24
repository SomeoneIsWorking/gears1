// The EDRAM-tiling collapse. gpu_draw_untile.h says what it is and why the
// console did the other thing; this file is the transformation.

#include "gpu_draw_untile.h"

#include <algorithm>
#include <map>
#include <string>

#include <lucent/log.h>

#include "gpu_draw_formats.h"

namespace gears::draw
{

void CollapseEdramTiling(std::vector<PreparedDraw> &prepared, uint32_t &issued,
                         bool scissorsAreSamples, bool reportDiagnostics)
{
    // A tile group is a maximal run of consecutive draws on one surface
    // sharing a window offset. Resolves delimit them.
    struct Group
    {
        uint32_t surface = 0;
        uint32_t windowOffset = 0;
        size_t first = 0, last = 0; // indices into `prepared`
        size_t drawCount = 0;
    };
    std::vector<Group> groups;
    for (size_t i = 0; i < prepared.size(); ++i)
    {
        const PreparedDraw &pd = prepared[i];
        if (pd.isResolve)
            continue;
        if (!groups.empty() && groups.back().surface == pd.surfaceBase &&
            groups.back().windowOffset == pd.windowOffset && groups.back().last + 1 >= i)
        {
            groups.back().last = i;
            ++groups.back().drawCount;
            continue;
        }
        Group g;
        g.surface = pd.surfaceBase;
        g.windowOffset = pd.windowOffset;
        g.first = g.last = i;
        g.drawCount = 1;
        groups.push_back(g);
    }

    // Does group `b` replay group `a` exactly? On failure it says WHY --
    // "10 candidates rejected" with no reason is a diagnostic that cannot
    // distinguish "this frame is not tiled" from "my grouping is wrong",
    // and the first version of this code could not tell those apart.
    std::map<std::string, uint32_t> rejectWhy;
    auto isReplayOf = [&](const Group &a, const Group &b)
    {
        if (a.surface != b.surface)
        {
            if (reportDiagnostics)
                ++rejectWhy["different surface"];
            return false;
        }
        if (a.drawCount == 0)
        {
            if (reportDiagnostics)
                ++rejectWhy["empty group"];
            return false;
        }
        if (a.windowOffset == b.windowOffset)
        {
            if (reportDiagnostics)
                ++rejectWhy["same window offset (not a tile replay)"];
            return false;
        }
        // A SUFFIX MATCH, not an equal-length one. The base tile's group
        // also carries the frame's one-off setup -- on the Act 1 courtyard
        // frame it starts with the colour clear, so it is 175 draws against
        // the replay's 174. Requiring equal counts rejected the only real
        // tiled surface in the frame, which is what the reject-reason line
        // was added to reveal.
        if (b.drawCount > a.drawCount)
        {
            if (reportDiagnostics)
                ++rejectWhy["replay longer than base (" + std::to_string(b.drawCount) + " vs " +
                            std::to_string(a.drawCount) + ")"];
            return false;
        }
        // Walk A from the point where its trailing b.drawCount draws begin.
        size_t ia = a.first, ib = b.first;
        for (size_t skip = a.drawCount - b.drawCount; skip != 0; --skip)
        {
            while (ia <= a.last && prepared[ia].isResolve)
                ++ia;
            ++ia;
        }
        for (size_t n = 0; n < b.drawCount; ++n)
        {
            while (ia <= a.last && prepared[ia].isResolve)
                ++ia;
            while (ib <= b.last && prepared[ib].isResolve)
                ++ib;
            if (ia > a.last || ib > b.last)
                return false;
            const PreparedDraw &x = prepared[ia];
            const PreparedDraw &y = prepared[ib];
            if (x.vsHash != y.vsHash || x.psHash != y.psHash || x.count != y.count ||
                x.primType != y.primType || x.indexed != y.indexed || x.colorMask != y.colorMask ||
                x.blend0 != y.blend0 || x.depthControl != y.depthControl)
            {
                if (reportDiagnostics)
                    ++rejectWhy["state differs at pair " + std::to_string(n)];
                return false;
            }
            ++ia;
            ++ib;
        }
        return true;
    };

    std::vector<bool> drop(prepared.size(), false);
    uint32_t collapsedGroups = 0, droppedDraws = 0, mergedResolves = 0;
    uint32_t examined = 0, rejected = 0;
    for (size_t gi = 0; gi + 1 < groups.size(); ++gi)
    {
        if (groups[gi].windowOffset != 0)
            continue;
        // Collect the consecutive groups on this surface that replay it.
        std::vector<size_t> replays;
        for (size_t gj = gi + 1; gj < groups.size(); ++gj)
        {
            if (groups[gj].surface != groups[gi].surface)
                break;
            ++examined;
            if (!isReplayOf(groups[gi], groups[gj]))
            {
                ++rejected;
                break;
            }
            replays.push_back(gj);
        }
        if (replays.empty())
            continue;

        // The union of every tile's resolve destination is what the base
        // tile must now cover. Resolves between/after the groups name it.
        int32_t dstBottom = 0;
        uint32_t dest = 0;
        size_t unionEnd = groups[replays.back()].last;
        while (unionEnd + 1 < prepared.size() && prepared[unionEnd + 1].isResolve)
            ++unionEnd;
        for (size_t i = groups[gi].first; i <= unionEnd && i < prepared.size(); ++i)
        {
            const PreparedDraw &r = prepared[i];
            if (!r.isResolve || r.resolveIsDepth || r.resolveDest == 0)
                continue;
            dest = r.resolveDest;
            dstBottom =
                std::max(dstBottom, r.resolveDstY + int32_t(r.resolveSrcRect.extent.height));
        }
        if (dest == 0 || dstBottom <= 0)
        {
            if (reportDiagnostics)
                ++rejectWhy["no colour resolve destination spanning the tiles"];
            ++rejected;
            continue;
        }

        // Widen the base tile's scissor to the full height it now draws.
        for (size_t i = groups[gi].first; i <= groups[gi].last; ++i)
        {
            if (prepared[i].isResolve)
                continue;
            // dstBottom is a row of the resolve DESTINATION, in pixels; the
            // scissor is in this draw's own space. Under the sample model
            // those differ by the surface's vertical sample scale, and
            // comparing them unconverted is a no-op for every 2X tile --
            // max(1024 samples, 720 pixels) leaves the band at 1024.
            const uint32_t sy =
                scissorsAreSamples ? MsaaScaleY((prepared[i].surfaceInfo >> 16) & 3) : 1u;
            prepared[i].scissor.extent.height =
                std::max(prepared[i].scissor.extent.height,
                         uint32_t(dstBottom) * sy - prepared[i].scissor.offset.y);
        }
        // Widen the base tile's colour and depth resolves to the union, and
        // drop the replays' draws and resolves.
        bool widened = false;
        uint32_t droppedHere = 0, mergedHere = 0;
        // THE BASE TILE'S RESOLVES COME AFTER ITS DRAWS -- they are what ends
        // the tile. So "belongs to the base tile" is everything before the
        // first REPLAY draw, not everything up to the base group's last draw.
        // Getting that boundary wrong made the collapse reject itself with
        // "no resolve at destination row 0" while having correctly identified
        // the replay, which is the sort of failure only a per-reason
        // diagnostic finds.
        const size_t firstReplay = groups[replays.front()].first;
        // ...and by the same token the LAST replay's resolves come after its
        // last draw. Stopping the scan at that draw left tile 2's resolves in
        // the stream, copying tile 1's top rows over the destination's bottom
        // band: the collapse reported success and the picture went dark.
        size_t scanEnd = groups[replays.back()].last;
        while (scanEnd + 1 < prepared.size() && prepared[scanEnd + 1].isResolve)
            ++scanEnd;
        for (size_t i = groups[gi].first; i <= scanEnd && i < prepared.size(); ++i)
        {
            PreparedDraw &r = prepared[i];
            const bool inBase = i < firstReplay;
            if (r.isResolve)
            {
                if (inBase && r.resolveDstY == 0)
                {
                    r.resolveSrcRect.extent.height = uint32_t(dstBottom);
                    widened = true;
                }
                else if (!inBase)
                {
                    drop[i] = true;
                    ++mergedHere;
                }
                continue;
            }
            if (!inBase)
            {
                drop[i] = true;
                ++droppedHere;
            }
        }
        if (!widened)
        {
            // The base tile has no resolve at the origin to widen, so the
            // collapse would lose the replays' output. Undo and leave it.
            for (size_t i = groups[gi].first; i <= scanEnd && i < prepared.size(); ++i)
                drop[i] = false;
            if (reportDiagnostics)
                ++rejectWhy["base tile has no resolve at destination row 0"];
            ++rejected;
            continue;
        }
        // Only now are the counters real. Incrementing them before the undo
        // above reported "0 collapsed, 174 dropped" -- a line that cannot be
        // true and that hid which of the two numbers was wrong.
        droppedDraws += droppedHere;
        mergedResolves += mergedHere;
        ++collapsedGroups;
        gi = replays.back();
    }

    if (collapsedGroups != 0)
    {
        std::vector<PreparedDraw> kept;
        kept.reserve(prepared.size());
        for (size_t i = 0; i < prepared.size(); ++i)
            if (!drop[i])
                kept.push_back(std::move(prepared[i]));
        prepared.swap(kept);
        issued -= droppedDraws;
    }
    lucent::Line summary;
    lucent::Line rejectionReasons;
    lucent::Line groupCensus;
    lucent::Line warning;
    if (reportDiagnostics)
    {
        summary.add("untile: {} tile group(s) collapsed, {} replayed draws"
                    " and {} resolves dropped; {} candidate group(s) examined, {} REJECTED"
                    " as not provable replays and left tiled",
                    collapsedGroups, droppedDraws, mergedResolves, examined, rejected);
        if (!rejectWhy.empty())
        {
            rejectionReasons.add("untile: why candidates were rejected:");
            for (const auto &kv : rejectWhy)
                rejectionReasons.add(" [{} x{}]", kv.first, kv.second);
        }
        // The group census, so "not a provable replay" can be checked against
        // what the grouping actually saw rather than believed.
        groupCensus.add("untile: {} draw group(s):", groups.size());
        for (const Group &g : groups)
            groupCensus.add(" [surf {:#x} wo {:#x} x{}]", g.surface, g.windowOffset, g.drawCount);
        if (collapsedGroups == 0)
            warning.add("untile: nothing was collapsed. Either this frame"
                        " is not tiled, or every candidate failed the replay test -- the"
                        " counts above say which, and NOT collapsing is the safe outcome");
    }
    summary.flush(lucent::Level::Info, "draw");
    rejectionReasons.flush(lucent::Level::Info, "draw");
    groupCensus.flush(lucent::Level::Info, "draw");
    warning.flush(lucent::Level::Warn, "draw");
}

} // namespace gears::draw
