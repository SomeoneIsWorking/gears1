#include "gpu_draw_census.h"

#include <cassert>

int main()
{
    gears::draw::FrameCensus silent(false);
    silent.NoteDraw(0x400, 8, 4);
    silent.NoteDepth(0x400, 0x200);
    silent.Skip(3);
    assert(silent.surfaces.empty());
    assert(silent.edramModes.empty());
    assert(silent.depthBases.empty());
    assert(silent.surfaceDepthPairs.empty());
    assert(silent.skipReasons.empty());
    assert(silent.skipped == 0);

    gears::draw::FrameCensus report(true);
    report.NoteDraw(0x400, 8, 4);
    report.NoteDepth(0x400, 0x200);
    report.Skip(3);
    assert(report.surfaces.at(0x400).draws == 1);
    assert(report.edramModes.at(4) == 1);
    assert(report.depthBases.contains(0x200));
    assert(report.surfaceDepthPairs.at({0x400, 0x200}) == 1);
    assert(report.skipReasons.at(3) == 1);
    assert(report.skipped == 1);
}
