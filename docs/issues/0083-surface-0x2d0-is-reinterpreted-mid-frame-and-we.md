---
id: 83
title: Surface 0x2d0 is reinterpreted mid-frame and we store values where EDRAM stores bits
status: resolved
symptom: a draw blends against 1.0 where the console reads 31.875; frame-wide red/green imbalance; the same EDRAM base declared under four bit layouts in one frame
tags: gpu,draw,edram,format,reinterpret,colour,act1,native-renderer
created: 2026-08-06
updated: 2026-08-06
---

## What was measured

An EDRAM base is a location, not a typed image. Surface `0x2d0` is declared
under five colour formats inside one Act 1 frame (`walk_gameplay.gfr`), and
`StorageColorFormat` collapses those to four distinct BIT layouts:
`k_8_8_8_8` x289, `k_2_10_10_10` x2, `k_2_10_10_10_FLOAT` x26+47, `k_16_16` x1.
The order, from `GEARS_DRAW_DIAG` with resolve rows excluded:

    draw 612 fmt 3   draw 613 fmt 4   draw 615 fmt 3   draw 616 fmt 12
    draw 640 fmt 0   draw 649 fmt 2   draw 650 fmt 12  draw 658 fmt 0
    draw 660 fmt 2   draw 662 fmt 12  draw 684 fmt 3   draw 716 fmt 0

Draw 649 is a full-screen `triangle_strip`, colour mask 15, 921600 fragments
shaded, no blend, `color_fmt` 2 = `k_2_10_10_10` -- a FIXED-POINT target, so the
hardware clamps its source colour and stores 10-bit UNORM bits. Draws 650, 651
and 655 declare `k_2_10_10_10_FLOAT_AS_16_16_16_16` on the same base and BLEND
(`blend_on` 1). On the console they read the bits `0x3FF` back as the 7e3 float
**31.875**; this renderer stores the value `1.0` it clamped to and reads `1.0`.

That closes the order question the previous session left open ("NOT YET
ESTABLISHED: that this frame's later draws actually reinterpret base 0x2d0 as a
float format after draw 649"). They do, immediately, and they blend against it.

## The pass, and what it does to the frame

`runtime/gpu_draw_reinterpret.cpp` + `runtime/shaders/edram_reinterpret.comp`
convert a surface's contents at every storage-format change: pack the stored
value under the old format, unpack under the new. `GEARS_DRAW_REINTERP=1`.
Bit layouts ported from Xenia's ownership transfer, 7e3 from its SPIR-V
translator. `GEARS_DRAW_REINTERP_SELFTEST=1` passes 5 of 5 on this GPU,
including the pair that must NOT change anything and a two-part round trip
(7e3 -> 8888 -> 7e3, the second half fed the first half's measured output),
which is the property that makes converting the WHOLE surface sound.

On `walk_gameplay.gfr`: 8 conversions, 2 refused (the `k_16_16` pair -- named,
not approximated). `GEARS_DRAW_AB=DRAW_REINTERP` puts the first divergence at
guest draw 640, the frame's first storage-format change.

`tools/frame_stats.py --diff`:

                      OFF                     ON
    R mean          0.0591                 0.3867
    G mean          0.0768                 0.3871
    B mean          0.0764                 0.3843
    R/G             0.7687                 0.9990
    p99             0.267                  1.000

**Catalog #62's headline -- "red is 78% of green frame-wide" -- disappears.**
The colour balance lands on the diagonal. And the picture blows out: large
regions saturate to white, with red/magenta fringing where the channel fields
of one format land across another's boundaries.

## What is NOT established

Whether the blow-out means the pass is wrong or means the pass is right and is
AMPLIFYING an upstream error. The mechanism is a multiplier: any pixel this
frame writes as 1.0 under a fixed-point format becomes 31.875 to the next
draw that blends. If our draw 649 shades brighter than the console's, the
reinterpretation turns a small error into a 31.875x one.

The round-trip self-test rules out the pass being destructive on untouched
pixels. What it cannot rule out is the pass being applied where the console
would not apply it -- it converts the whole surface at the format change, where
the hardware only reinterprets on a READ.

Next: the Xenia oracle renders this scene with a faithful per-(base, format) RT
cache. If its frame at the same moment is not blown out, the fault is upstream
of the reinterpretation, and #62 and #81 are the places to look.

### Note (2026-08-06)
## The oracle answers the open question: the pass amplifies, it does not invent

`scratch/oracle/compare/theirs/frame_0210s.png` is Xenia at the same Act 1 wall.
Xenia keys render targets on (base, format) and performs exactly this
reinterpretation as an ownership transfer -- it is the code this pass is ported
from -- and its frame is DARK and correctly exposed: mean 22.1, 24,497 distinct
colours (catalog #77). Nothing saturates except the sky inside the window
frames.

So a faithful reinterpretation is compatible with a correct picture, and our
blow-out is not the mechanism being wrong. It is the mechanism multiplying
something we already get wrong upstream: catalog #77 measures our frame as
BRIGHTER than the oracle's with a third of the colour variety even with the
pass OFF (mean 30.3 vs 22.1, 6,711 colours vs 24,497) -- the signature of a
flattened tonemap. Any pixel that reaches 1.0 under a fixed-point format becomes
31.875 to the next draw that blends, so a flattened tonemap and this pass
together produce exactly the white slabs seen.

Caveat on the strength of this: the two frames are different moments (#77 says
why no pixel metric is quoted between them). "Not blown out" survives that; a
per-pixel claim would not.

CONSEQUENCE FOR ORDER OF WORK. #62 (colour flatness) and #81 (black bloom) are
upstream of this, and the R/G 0.769 -> 0.999 result says #62's frame-wide red
deficit is at least partly a symptom of the missing reinterpretation rather than
an independent fault. The pass should be re-measured after the post chain is
right, not turned on to fix the post chain.

### Note (2026-08-06)
## FALSIFIED: the R/G evidence for this pass was the SWIZZLE BUG, not the reinterpretation (2026-08-06)

This entry, and commit c344b48 with it, recorded that turning the pass on moves
catalog #62's frame-wide colour balance onto the diagonal:

    OFF  R/G 0.7687        ON  R/G 0.9990

and concluded "#62's frame-wide red deficit is at least partly a symptom of the
missing reinterpretation rather than an independent fault".

That is wrong, and the cause is now fixed elsewhere. #62's red deficit was
resolve-target bindings ignoring the guest's fetch swizzle. With that fixed and
this pass still OFF, on the same capture:

    walk_gameplay, REINTERP OFF   R/G 0.9937  B/G 0.7687   mean 0.076
    walk_gameplay, REINTERP ON    R/G 0.9928  B/G 0.9990   mean 0.384
    ORACLE, Act 1                 R/G 0.9882  B/G 0.7237   mean 0.095

The diagonal was already there without the pass. Worse for the old reading:
turning the pass ON now makes B/G 0.999 where the oracle says 0.724, so it
DESTROYS a ratio that is otherwise correct, and lifts the mean 5x above the
reference. The pass was being credited for fixing a fault it had nothing to do
with, and its damage was hidden behind that credit.

Nothing else in this entry is retracted: the surface really is declared under
four bit layouts in one frame, draw 649 really is a fixed-point target, the
self-test still passes 5 of 5, and the oracle really does render this scene dark
through a faithful ownership transfer.

## What the pass IS still the candidate for, now measured precisely

The ceiling, and only the ceiling. Pixel trace of the frame's brightest scene
pixel (368,247), pinned per surface:

    surface 0x400   draw 347  (37.09, 30.98, 15.91)      the base pass, real HDR
    surface 0x2d0   draw 615  (37.09, 30.98, 15.91)      carried across intact
    surface 0x2d0   draw 649  (1, 1, 1)                  CLAMPED, and faithfully
    surface 0x2d0   draw 716  (0.297, 0.297, 0.269)      what we present

Draw 649's target is fixed-point, so the hardware clamps there too -- that step
is correct. The console's next draws then read those same EDRAM bits under a
FLOAT layout and see 31.875 where we see 1.0, which is this entry's whole thesis
and is exactly the missing top of #62's range.

So the mechanism is right and the implementation overshoots. The shape of the
overshoot is the clue and it is the one this entry already named as un-ruled-out:
the pass converts the WHOLE surface at a format change, where the hardware
reinterprets only on a READ. The numbers now say so quantitatively -- the oracle
holds mean 0.095 with p99 0.808, i.e. the same mean as ours-with-the-pass-off but
a far wider top end. Converting every pixel raises the mean 5x; reinterpreting
only what is actually read back as float would raise the top and leave the mean
alone. That is the difference between the two arms, and it is measurable.

NEXT: establish which draws actually READ 0x2d0 after each format change and
over what region, rather than converting the surface wholesale.

### Note (2026-08-06)
## MEASURED: the float-format draws cover 29% of the screen; we convert 100% of it

This entry has said since it opened that the un-ruled-out risk is "the pass being
applied where the console would not apply it -- it converts the whole surface at
the format change, where the hardware only reinterprets on a READ". That is no
longer a suspicion. From `GEARS_DRAW_DIAG` on `walk_gameplay.gfr`:

    draw  color_fmt  prim            frag_invocations  blend_on
    649   2          triangle_strip  921600            0      <- full screen, FIXED point
    650   12         triangle_list     79253           1
    651   12         triangle_list     42564           1
    655   12         triangle_list    148267           1
    664   12         quad_list         55949           1

The draws that declare the FLOAT layout are scattered geometry with blending and
touch 270,084 fragments between them -- 29% of the screen. Only draw 649, the
fixed-point write, is full-screen. Our pass converts all 921,600 pixels at the
format change, so the other 71% are lifted into the 7e3 interpretation by a read
that never happens.

## The quantiles say the same thing, and bracket the reference

Green channel, `walk_gameplay.gfr` against the oracle's Act 1:

                        median   p90     p99     p99.9   max
    ours REINTERP OFF   0.051    0.188   0.294   0.298   0.329
    ours REINTERP ON    0.141    1.000   1.000   1.000   1.000
    ORACLE              0.063    0.176   0.784   1.000   1.000

**OFF is already correct up to p90** -- median 0.051 against 0.063, p90 0.188
against 0.176 -- and then flatlines where the reference keeps going. ON is
correct only at the very top and saturates everything from p90 upward, doubling
the median.

So the bottom 90% of the frame needs NO reinterpretation and currently gets one;
the top 10% needs it and currently cannot have it, because turning it on ruins
the rest. The reference sits between the two arms, which is what a
whole-surface conversion of a partial-coverage effect looks like.

## What this does NOT yet establish

WHY converting a pixel that is never read under the float layout changes the
final image at all. The conversion is bit-exact and its round-trip self-test
passes, so a pixel converted to float and back should return to its own value --
and the trace of the saturated pixel (368,247) does round-trip: draw 649 (1,1,1),
650 31.875, 658 (1,1,1), 662 31.875. The frame ends with the surface left in the
float interpretation, and the final composite at draw 716 reads it there.

That is the thread to pull next, and it is a different question from the one
this note answers: not "is the coverage wrong" (measured: yes) but "what does the
surface's interpretation need to BE at draw 716". #83's format list has draw 716
at fmt 0, i.e. back to k_8_8_8_8 -- so if our tracking leaves it float at that
point, the final pass reads through the wrong layout and that alone would produce
this. CHECK THAT FIRST; it is cheaper than reworking the coverage.

### Note (2026-08-06)
## The "left in the float interpretation at 716" hypothesis is DEAD, and bloom needs this pass

### Killed, by one column of the diag table

The previous note said to check first whether our format tracking leaves 0x2d0
in the float interpretation at draw 716, where the guest declares k_8_8_8_8. It
cannot matter:

    draw  color_fmt  prim_name      frag_invocations  color_mask  blend_on
    716   0          triangle_list  921600            15          0

**blend_on 0.** Draw 716 is a pure full-screen write that samples textures and
never reads its own render target, so whatever interpretation 0x2d0 is left in
immediately before it is discarded. Recorded because it was this entry's stated
next step and it is an hour nobody else needs to spend.

(Also corrected: draws 708-714, which I read as a k_16_16_16_16_FLOAT phase on
0x2d0, are on surface 0x5a0 -- the frame report's "8@0x5a0:f7" says so. They are
the bloom chain, not part of 0x2d0's format sequence.)

### So the ceiling propagates through the RESOLVE TARGETS 716 samples

Resolve destinations at end of frame, both arms, same capture:

                    REINTERP OFF                  REINTERP ON
    0xbdf0000       0..0.305   99.3% non-zero     0..3.984   99.3%
    0xc7f9000       0..0.331   99.3%              0..1.000   99.3%
    0x6e4000        0..0.050    1.6%              0..3.586   32.2%
    0x311000        0..0.331  100.0%              0..1.000   99.3%

### THE PASS IS NOT OPTIONAL: bloom only exists with it on

0x6e4000 is the bloom resolve destination. With the pass OFF it is 1.6% non-zero
with a maximum of 0.05 -- which is catalog #81's "the whole bloom chain renders
black", still true today and NOT caused by anything else. With the pass ON it is
32.2% non-zero reaching 3.586.

#81 derived a quantitative target for this: the bright pass thresholds with sgt
against 1.0 and its input carries copy_dest_exp_bias -3, so the source must
reach 8.0 for bloom to exist at all. OFF we reach 0.305; ON we reach 3.98. Still
short of 8.0, but thirteen times closer, and the effect goes from nothing to a
third of the buffer.

So #81 and this entry are the same defect seen from two ends, and the
reinterpretation is REQUIRED rather than a candidate. That reframes the work:
the question is not "should this pass exist" but "why does applying it
over-brighten the mid-tones", with the coverage measurement from the previous
note (29% of the screen declares the float layout; we convert 100%) as the
leading answer.

### Honest status

Neither arm is shippable and the entry stays `investigating`. OFF is correct to
p90 and has no highlights and no bloom; ON has highlights and bloom and is 2.2x
too bright at the median. The reference lies between them, and no global scale
moves one arm onto it -- which is why this is a coverage question, not a
tuning one.

### Note (2026-08-06)
## BUILT: the pass no longer converts for draws that do not read -- and it did NOT fix the brightness

### What was built

`gpu_draw.cpp`'s reinterpretation trigger fired on EVERY format change. Its own
comment said "convert before the draw that reads them back", but the condition
never tested whether the draw reads. It now does: a resolve reads by definition,
and a geometry draw reads only when its blend is not the identity -- the same
`BlendIsIdentity(blend0)` predicate the pipeline uses to enable blending. The
declined conversions are counted and printed with their format pairs, so a
reader can tell a frame with three changes from one with eight where five were
correctly declined.

On `walk_gameplay.gfr` that is 2 conversions where there were 10:

    2 converted k_2_10_10_10->k_2_10_10_10_FLOAT,
    8 NOT converted because the draw meeting the format change does not read
      the destination

This is the faithful model -- the hardware converts nothing at a format change;
the bits sit in EDRAM and whoever reads them next interprets them -- and it
removes eight full-surface compute dispatches per frame.

### AND IT CHANGED NOTHING. My hypothesis was wrong.

The presented frame is BYTE-IDENTICAL to the convert-everything arm, and the
quantiles are unmoved:

    ON, convert-everything      median 0.141  p90 1.000  p99 1.000
    ON, convert-on-read (new)   median 0.141  p90 1.000  p99 1.000
    ORACLE                      median 0.063  p90 0.176  p99 0.784

So the over-brightening does NOT come from converting at draws that never read.
It comes entirely from the two conversions that remain -- which are legitimate
reads under any model. The previous note's framing ("29% declares the float
layout, we convert 100% of it") was right about the coverage and wrong about
which conversions cause the damage.

### The sharpest evidence yet: a mid-tone pixel that no reading draw covers

`GEARS_DRAW_PIXEL_TRACE=640,350` (bare wall), pinned to 0x2d0:

    OFF   draw 615  (0.535, 0.627, 0.799)  ->  draw 716  (0.205, 0.218, 0.208)
    ON    draw 615  (0.535, 0.627, 0.799)
          draw 650  (2.547, 4.031, 11.125)     <- lifted ~14x
          draw 660  (1, 1, 1)
          draw 716  (1, 1, 1)

Draw 650 covers 79,253 fragments -- 8.6% of the screen -- and this wall pixel is
not among them. The whole-surface conversion lifted it anyway, and the resolves
at draws 657 and 659 read the lifted surface before anything restores it. That
is the entire defect, in one pixel.

### So what is actually needed, and why it is not a small change

The conversion must not touch pixels the reading draw does not cover. The
reading draws are scattered geometry, not rectangles, so their coverage is
per-fragment -- which means the reinterpretation has to happen INSIDE the
reading draw's blend, not as a surface-wide pre-pass. That is what fragment
shader interlock exists for and is why Xenia has an FSI path at all; our
renderer models the host-render-target path, where this cannot be expressed
exactly.

A bracket (convert in before the reading run, convert back after it) looked like
the cheap answer and is NOT one: the resolves at 657/659 sit inside the run and
read the lifted surface, so a restore afterwards is already too late.

DO NOT try a third variation of surface-wide conversion. Two have now been
measured and both fail for the same reason. The next real step is either
per-fragment reinterpretation at read time, or establishing from the guest's
register stream that the resolves at 657/659 read under a format that makes the
lift correct -- in which case the fault is downstream of here and this pass is
innocent.

### Status

The pass stays OFF by default; the REINTERP-OFF arm is byte-identical to before
this change, and validate_all and verify_native_pass both pass. The change is
kept because it is the more faithful model, is fully reported, and removes work
-- not because it fixed anything. It did not.

### Note (2026-08-06)
## RESOLVED: the resolve read its source format from the WRONG REGISTER (2026-08-06)

### The root cause, in one line

A resolve reads its source EDRAM under `RB_COLOR_INFO[RB_COPY_CONTROL.copy_src_select]`.
We read `RB_COLOR_INFO0` unconditionally. Every resolve in the frame therefore
reported colour format 0 -- including the bloom copy whose own draws are
`k_16_16_16_16_FLOAT` -- and the uniformity of that column is the tell that was
sitting in the diag table the whole time.

Xenia indexes the same four registers (`draw_util.cc` `GetResolveInfo`, and the
registers are NOT contiguous: 0x2001, 0x2003, 0x2004, 0x2005). Our own resolve
DECODE already did this correctly for `srcBase` two lines earlier; only the
format was taken from RT0.

### Why that produced the blow-out

With the source format wrong, a resolve either converted the surface to an
interpretation the copy did not read it under, or -- because the conversion was
never triggered for resolves at all -- copied out a surface still sitting in the
float interpretation a blending draw had left it in. Catalog #62's wall pixel
(640,350) went to (2.547, 4.031, 11.125) at draw 650 and the resolves at 657 and
659 carried that away before anything restored it.

Three things had to be right together, and each was measured on its own:

1. **Convert only before a READ.** The hardware converts nothing at a format
   change; the bits sit in EDRAM and whoever reads them next interprets them. A
   draw that does not read (blend is the identity) never sees the old bits, and
   converting for it rewrites every pixel it does not cover. 8 of 12 changes in
   this frame are now correctly declined, and they are counted and named.
2. **A resolve IS a read**, and the resolve branch `continue`s long before the
   trigger, so it needed its own call. Adding `isResolve` to the trigger's
   condition was DEAD CODE -- recorded because it looked right.
3. **A resolve reads under its own source format**, above.

With (1) and (2) but not (3), the blow-out went but the mid-tones stayed 2.7x
bright. All three: the frame gains the top of its range and keeps its mid-tones.

### The result, green channel, walk_gameplay.gfr

                        median   p90     p99     p99.9   max
    before (pass off)   0.051    0.188   0.294   0.298   0.329
    AFTER               0.051    0.192   0.549   0.859   1.000
    ORACLE              0.063    0.176   0.784   1.000   1.000

The median does not move, p90 lands on the oracle's, and the top of the range
comes back where there was none. Channel balance is preserved: R/G 0.9934,
B/G 0.7816 against the oracle's 0.9882 and 0.7237, and `chroma_compare` still
reports IDENTITY on 4 of 4 oracle pairs, inside the null band.

Visibly: the windows carry daylight (the rightmost shows foliage through it)
against a properly dark room, where they were flat grey slabs. That is #77's
difference 3.

### Now ON by default

The reason it shipped off -- "it blows the picture out" -- was this bug, not the
mechanism. `GEARS_DRAW_NOREINTERP=1` is the control arm and reproduces the old
output byte for byte. 4 conversions on this frame, **0 refused** (the refusals
were themselves an artefact of the wrong source format asking for pairs the pass
does not implement).

Gates: `validate_all` PASS on all 8 captures; `verify_native_pass` PASS
bit-exact with its interface arm clean and its negative control still firing;
no capture blows out or goes black (`play_v2`'s zero is pre-existing and
reproduces with the pass off).

### What this does NOT close

p99 0.549 against the oracle's 0.784 -- we still under-reach at the top. That is
a remaining gap, not a regression, and it belongs to #62 rather than here.
#81 (bloom) should be re-measured now: this is the change that gives it a
non-trivial input.
