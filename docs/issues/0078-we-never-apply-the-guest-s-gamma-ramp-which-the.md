---
id: 78
title: We never apply the guest's gamma ramp, which the hardware and the oracle both do
status: resolved
symptom: our presented frame is flatter than the reference: a third of the distinct colours, and brightness that does not match
tags: render,colour,present,oracle
created: 2026-08-05
updated: 2026-08-20
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

### Note (2026-08-06)
## IMPLEMENTED, and the claim about Xenia was WRONG (2026-08-06)

Two corrections to the entry above, then the result.

CORRECTION 1. "Xenia implements only the SEQ_COLOR path" is FALSE. It handles
DC_LUT_30_COLOR too, in command_processor.cc's WriteRegister, with the same
index auto-increment. The error came from a grep truncated at ten hits that cut
off before that case. It matters because the entry used it to argue the oracle
was not applying this title's ramp either -- the opposite is true, and that is
exactly why implementing the ramp moved our output TOWARD the reference.

CORRECTION 2. The first implementation here reported "1 of 256 entries differs
from linear", which read as "this title's ramp is a no-op". That was a defect in
this code: DC_LUT_30_COLOR AUTO-INCREMENTS the write index, and without that
every write of an upload landed in entry 0, leaving 255 entries at their default
and entry 0 holding the last value. With the increment: 254 of 256 entries
differ. The register file's end state is what gives the increment away -- an
8-bit index wraps to 0 after exactly 256 writes, which is what a capture taken
later shows. Xenia's own case, found afterwards, does the same thing.

## What the title actually uploads

Measured on a live headless run: 256 writes, ALL whole-entry via
DC_LUT_30_COLOR, zero sequential, zero PWL. Not programmed at boot -- it appears
later, so an instrument that only looks at the first presented frame reports a
confident "none" (the first version of this one did, and now says so).

The ramp is markedly non-linear and darkens the low end: entry 1 maps to 1 of
1023 where linear gives 4, entry 4 to 6 where linear gives 16 -- roughly a
gamma of 1.24 on top of the composite's own output.

## Implemented, and the effect measured

The command processor accumulates the ramp as the writes go past (it cannot be
recovered from a register snapshot) and hands it to the renderer, which applies
it per channel on the way to host pixels. On the same scripted walk, our own
gameplay frames:

    before   mean 30.3   6,711 colours   95.1% of pixels above 8/255
    after    mean 17.4   5,807 colours   71.6%
    oracle   mean 22.1  24,497 colours   75.8%

So the brightness difference against the reference is largely accounted for, and
the image is not crushed -- wall texture, moss and window frames keep their
detail.

WHAT THIS DOES NOT FIX, stated because the entry above guessed it might: the
COLOUR VARIETY gap is untouched (5,807 against ~24,000). Whatever flattens our
composite is a separate defect, and the gamma ramp is no longer a candidate for
it.

NOT COVERED: the shared-device present path, where the presenter blits the
rendered image straight to the swapchain without going through these host
pixels. Headless runs, screenshots and the census all get the ramp; a window can
still look brighter. The renderer says so in its per-frame report rather than
leaving the two paths silently different.

### Resolution (2026-08-20)
The shared-device gap is closed. GpuScanout now applies the guest 256-entry LUT in place on the alternating RGBA8 scan-out images before publishing them, while the CPU path uses the same BuildScanoutGammaLut conversion and skips a second transform when GPU gamma already ran. A live validation run observed 256 writes with 254 non-linear entries, built the scan-out gamma pipeline, raised no Vulkan validation messages, and captured the presented title frame at mean channel brightness 17.4, matching the previously measured CPU-gamma path. Focused LUT and source-structure tests plus clang-tidy pass.
