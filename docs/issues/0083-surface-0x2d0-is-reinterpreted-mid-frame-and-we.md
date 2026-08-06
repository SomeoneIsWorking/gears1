---
id: 83
title: Surface 0x2d0 is reinterpreted mid-frame and we store values where EDRAM stores bits
status: investigating
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
