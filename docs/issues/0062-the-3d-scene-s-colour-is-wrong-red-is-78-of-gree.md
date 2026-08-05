---
id: 62
title: The 3D scene's colour is wrong: red is 78% of green frame-wide, and lit surfaces flatten at 0.30
status: open
symptom: Gameplay looks flat grey-green with blown, detail-free lit surfaces; menus and the title screen look correct in the same run
tags: gpu,draw,colour,tonemap,resolve
created: 2026-08-05
updated: 2026-08-05
---

## The observation that redirects everything

The reported title-screen capture is CORRECT -- dark blue, proper contrast, crisp
text. Only the 3D scenes are wrong, in the same run. That rules out every
whole-frame explanation I pursued for two days: a swapchain format, a colour
space, a compositor transform and an sRGB encode would all wash the menus too.

It is content-specific, which means it is in the part of the pipeline the menus do
not use: the deferred scene path, its HDR surface, the resolve that carries it and
the tonemap that consumes it.

## Measured on our own presented gameplay frame

Whole frame, per channel:  R 0.0772  G 0.0996  B 0.0990
                           -> RED IS 78% OF GREEN, and green and blue are equal.

The sunlit wall through the doorway: mean 0.193, p99 0.298, per-channel
R 0.174 G 0.204 B 0.200. A daylit concrete wall is neutral or warm; this is
uniformly cool, and it flattens at 0.30 with no highlight above it.

Two separate defects in one number: a RED DEFICIT of about a fifth, and a CEILING
that a lit surface cannot pass.

## What this retracts

Every "the renderer's output is correct" in this session was my own judgement of a
dim scene against no reference. It was wrong. The frames I called correct have the
same cast and the same ceiling as the reported screenshots -- the reports were just
of brighter scenes where it is obvious. Catalog #60 and #61 chased the difference
between my captures and the window when the interesting difference was between
BOTH of them and the console.

## Where to look, in order

1. The resolve's `copy_dest_swap` and the exponent bias -- catalog #33 found the
   bias wrong once already (the tonemap's input was 8x too bright), and a red/blue
   asymmetry is exactly what a swizzle or a per-channel scale gets wrong.
2. The 7e3 surface format carried as half-float: an unequal-precision packing would
   hit one channel differently.
3. The tonemap shader's own constants, read from the register file.

An oracle settles it in one comparison and there is one available: Xenia canary
renders this title correctly (user's report), and `extern/xenia` is already
vendored. A single frame of its output next to ours ends the guessing that this
entry exists because of.

### Note (2026-08-05)
## The red deficit is a RED/BLUE SWAP, and it is 100% of pixels

Not a tonemap, not an exponent bias, not a 7e3 packing. The frame we present is
an exact channel exchange of the frame the guest composed.

`GEARS_DRAW_RESOLVE_DUMP=1` now reports **per-channel means** for every resolve
target (new; the old line's range was a max over all three channels, which is
identical whether red is short or not). On courtyard.gfr:

    resolve 0x311000   R 0.076818  G 0.077251  B 0.059357     <- the FRONT BUFFER
    resolve 0xc7f9000  R 0.059357  G 0.077251  B 0.076818
    presented frame    R 0.059357  G 0.077251  B 0.076818

Same three numbers, to six digits, with R and B exchanged. Confirmed per pixel,
not by aggregate:

    presented vs resolve 0xc7f9000 : identical             100.0% of pixels
    presented vs resolve 0x311000  : identical after R/B swap 100.0% of pixels

The capture's `frontBufferAddress` is 0xa0311000 in all three captures, so
0x311000 is the buffer the guest hands to scanout. **What we present is not it.**

R/G = 0.7688 was never a red deficit: red is carrying blue's level. The
"ceiling at 0.30" is a separate observation and is NOT explained by this.

## Which resolve swaps, measured with the existing control arm

`GEARS_DRAW_RESOLVE_NOSWAP=1` changes 0xc7f9000 (R 0.059357 -> 0.076818, i.e.
it matches 0x311000) and leaves 0x311000 untouched. So the guest asks for
`copy_dest_swap` on the resolve to 0xc7f9000 and NOT on the one to the front
buffer, and our resolve honours both correctly. The swap in the presented image
does not come from the resolve.

Both host formats are RGBA -- resolve targets are all R16G16B16A16_SFLOAT
(gpu_draw_xlate.cpp) and the readback is R8G8B8A8 -- so component 0 really is
red in both dumps and the channel labels above are not a BGRA mislabel.

## Next

The presented image is byte-identical to the 0xc7f9000 TEXTURE, which is a
resolve destination, not the surface the front buffer names. So the question is
in the present path, not the colour path: does it present the wrong image, or
does the 0xc7f9000 resolve write into the host image that gets presented?
Read the presentBase selection in gpu_draw.cpp (~line 1730) and what the
readback at ~1866 actually copies from.

Do not re-open the tonemap, the exponent bias or the 7e3 packing on this
evidence: a per-pixel 100% match cannot be produced by any of them.

### Note (2026-08-05)
## RETRACTED: the note above is WRONG where it concludes a swap DEFECT

The measurements in it are right. The inference from them is not, and the
entry must not be read as "the present path swaps channels".

**What is still true.** The presented frame is byte-identical to resolve target
0xc7f9000 and an exact R/B exchange of resolve target 0x311000, per pixel,
100.0% both ways. Those numbers stand.

**What is false.** That this shows a defect, and that it explains R/G = 0.7688.

Three things refute it, each measured after the fact:

1. **Both resolves ask for the swap.** The diag table now carries
   `resolve_swap_rb` (new column): draw 717 -> 0xc7f9000 and draw 743 ->
   0x311000 BOTH have swap_rb = 1. The earlier reading -- "the guest asks for
   the swap on one and not the other" -- came from the NOSWAP control arm
   moving only one of the two, and that is not what it means.
2. **Nothing fell back to the blit.** `resolvesUnstorable` is 0 on this frame,
   so no resolve silently dropped the swap; every one ran the compute path.
3. **The guest exchanges the channels itself, mid-frame.**
   `GEARS_DRAW_PIXEL_TRACE=835,258` on consecutive draws:

       after 527 draws (surface 0x2d0) = (0.296875, 0.296875, 0.2685547, 0) <- draw 702
       after 528 draws (surface 0x2d0) = (0.2685547, 0.296875, 0.296875, 0) <- draw 703

   Same pixel, same surface, R and B exchanged by ONE of the title's own draws.

That closes the loop consistently: 0xc7f9000 resolves the surface before draw
703, 0x311000 resolves it after, both applying the swap, and the presented
surface is the later state. Every relationship I measured follows from the
guest's own stream, and none of it requires a defect. It also agrees with the
present path's own reasoning in gpu_draw.cpp (~line 1797), which says
presenting the source surface rather than the front buffer is deliberate and
was settled against the boot movie -- I should have read that BEFORE concluding.

## So where #62 stands

Unchanged from before that note. R/G = 0.7688 and the ~0.30 ceiling both still
reproduce on today's build (courtyard.gfr: R 0.0594 G 0.0773 B 0.0768, p99
0.29), and the cause is NOT established. **The tonemap, the exponent bias and
the 7e3 packing are NOT ruled out** -- the previous note said they were, and
that exclusion rested on the same bad inference.

The oracle comparison the original entry asked for is still the thing that
would settle it, and nothing since has replaced it.

## What the detour did leave behind

Two instruments that stay useful and are validated:
- `GEARS_DRAW_RESOLVE_DUMP=1` reports per-channel means and R/G, B/G per
  target. The old line's range was a max across all three channels and reads
  identically whether one channel is short.
- The diag table has `resolve_swap_rb` and `resolve_scale`, so what a resolve
  was ASKED to do is readable without toggling a knob and diffing images.

### Note (2026-08-05)
## The oracle: what is ready, and what needs a person

The entry has said since it opened that an oracle settles this in one
comparison. Scoped this session; the blocker is not the build.

READY, nothing to do:
- Xenia is built on this machine and launches: `scratch/oracle/xenia-canary/
  build/bin/Linux/xenia_canary` (19 MB, catalog #7 has the three local
  workarounds and the exact configure line).
- The title's data is extracted at `scratch/game` (`default.xex` present), so
  it can be loaded directly -- no disc image step.
- `tools/frame_stats.py` (new) reports our frame and a reference frame with the
  SAME statistics from one command, PPM or PNG, so the comparison is not two
  ad-hoc scripts with two definitions. `--selftest` proves it reports both
  answers. It reproduces this entry's numbers on courtyard.gfr:
  R/G 0.7684, and a p99.9 of 0.298 against a max of 1.0 -- the 0.30 ceiling is
  a real ceiling with a handful of pixels above it, not a soft rolloff.

NEEDS A PERSON: reaching a gameplay scene in Xenia takes a window and a
controller, and that is an interactive session rather than something a
measurement run can do. Note `xenia_canary --help` does NOT print help and
hangs (it went to a UI picker); kill it by PID.

So the remaining step is one short interactive Xenia session that reaches any
Gears gameplay scene and takes a screenshot. It does NOT have to match one of
our captures: the question is whether a correct renderer of this title also
puts red a fifth below green frame-wide with a ~0.30 ceiling. If it does, the
premise of this entry is wrong and the frames were always right. If it does
not, the entry's three suspects are back on with a reference to aim at.

Then: `tools/frame_stats.py --diff <ours.ppm> <xenia.png>`.

### Note (2026-08-05)
## The oracle plan, corrected: build a harness, do not drive the emulator

The earlier note here said the remaining step was "one short interactive Xenia
session". That was wrong about what is needed. Xenia ships a HEADLESS renderer
for exactly this -- `xenia-gpu-vulkan-trace-dump`, which loads a GPU trace,
renders it and writes the frame with no window and no controller. It now builds
(catalog #7 has the one-line recipe and the artifact path).

So the shape is: OUR capture -> a Xenia GPU trace -> their renderer -> a PNG ->
`tools/frame_stats.py --diff`.

## The caveat that decides how much the answer is worth

A `.gfr` stores per-draw REGISTER SNAPSHOTS, decoded microcode and guest pages.
A Xenia trace stores a PM4 PACKET STREAM that their command processor executes.
We do not keep the packets, so a converter has to SYNTHESISE a command stream
from our snapshots -- which means encoding our own reading of the registers into
the input.

That is fine for what #62 asks (does a correct renderer of these draws put red a
fifth below green?) because the question is about SHADING, RESOLVE and FORMAT,
downstream of the decode. It is worthless for any question about command-stream
decoding: on those the oracle would agree with us by construction, exactly where
we are wrong. Say which kind of question is being asked before trusting a run.

The alternative with no such caveat is a trace RECORDED from Xenia running the
title, which needs the game played once. Strictly better evidence, higher cost.
Do not conflate the two arms or report one as if it were the other.
