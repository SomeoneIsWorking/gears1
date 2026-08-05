---
id: 78
title: We never apply the guest's gamma ramp, which the hardware and the oracle both do
status: open
symptom: our presented frame is flatter than the reference: a third of the distinct colours, and brightness that does not match
tags: render,colour,present,oracle
created: 2026-08-05
updated: 2026-08-05
---

Found by walking the oracle comparison (#77) inward with the frame-step probe,
on `scratch/frames/courtyard.gfr` replayed offline.

## How the frame builds, and where it flattens

`GEARS_DRAW_FRAME_STEP` on the captured frame, counting distinct colours in the
presented surface after every draw:

    after 519..521   184,909 colours, mean 46.16   the lit HDR scene
    after 522        1 colour                      target switches, cleared
    after 524..526   ~100 colours, mean 0.01       composite being built
    after 527        8,794 colours, mean 18.14     final, and it never changes

So the composite pass (draws 522-527) is where the frame's colour variety drops
by a factor of twenty. The reference's equivalent frame carries ~24,000 colours;
ours carries 8,794. The step at 522 is a legitimate surface switch, not a loss --
the loss is what the composite produces.

## The step we skip

Xenia applies the guest's GAMMA RAMP when it swaps
(vulkan_command_processor.cc IssueSwap: it uploads either the 256-entry table or
the PWL ramp, chosen by the front buffer format, and samples it while drawing the
guest output). We apply nothing: `VdGetCurrentDisplayGamma` reports 2.0 and the
present path never touches a ramp. There is no DC_LUT handling anywhere in the
runtime.

THE GUEST PROGRAMS ONE, measured in the captured frame's own register file:

    DC_LUT_WRITE_EN_MASK  0x1927 = 0x00000007   all three channels enabled
    DC_LUTA_CONTROL       0x1930 = 0x00000000   256-entry unsigned fixed point
    DC_LUT_30_COLOR       0x1925 = 0x3FFFFFFF   an entry was written

which matches Xenia's own note that Direct3D 9's InitializePresentationParameters
initialises the 256-entry table for 8_8_8_8 output.

## What this does and does not establish

It establishes that the hardware performs a per-channel LUT at scan-out that we
do not perform, and that this title uploads one. It does NOT establish that the
ramp alone accounts for the 8,794-vs-24,000 difference -- that needs the ramp
implemented and re-measured, and the composite may be losing precision for its
own reasons as well.

## What implementing it needs

  * the command processor must CAPTURE the ramp: it arrives as a stream of
    DC_LUT_30_COLOR writes indexed by DC_LUT_RW_INDEX, so the register file's
    last-value-wins snapshot cannot reconstruct it (this is why the .gfr, which
    stores register snapshots, cannot carry it either -- the capture format needs
    a ramp block, as Xenia's trace format has one);
  * the present path must sample it, per channel, with the table/PWL choice made
    by the front buffer format as the hardware does.
