---
id: 95
title: Two k_2_10_10_10 copies of the scene surface are near-black where the console has content
status: resolved
symptom: layer_compare srcC2D0 1280x720 f7 #0/#1: ours 0.0151, console 0.1760, in BOTH the pixel and the sample model
tags: oracle,layer-compare,resolve,reinterpret
created: 2026-08-11
updated: 2026-08-11
---

## What was measured

Two copies of surface 0x2d0 to a k_2_10_10_10 destination -- diag draws 639 and
657, which BRACKET the shadow-mask pass (the fill at 640 sits between them, and
658 is the second loop's fill) -- come out near-black on our side:

    pass                     ours     console
    srcC2D0 1280x720 f7 #0   0.0151   0.1760
    srcC2D0 1280x720 f7 #1   0.0151   0.1761

Both numbers are UNCHANGED by the EDRAM sample model, which moved every other
row it touched (catalog #91, #94). So this is not the sample count, and it is
not the shadow-mask fill either, even though these copies sit either side of it.

## What is known about them

* Both are 1X, sample 0, scale 1.0, no red/blue swap (`resolve_scale` 1,
  `resolve_swap_rb` 0, `copy_sample_sel` 0 in the draw table).
* Their destination, 0xc7c0000, is ALSO the destination of three f32 copies in
  the same frame -- so the same guest memory is resolved under two different
  formats, which is the situation EDRAM format reinterpretation exists for.
* Our side is 0.0151 for both, i.e. the same near-black buffer twice, while the
  console has 0.176 both times.

## Where to start

The destination being shared with a k_16_16_16_16_FLOAT copy makes the
reinterpretation pass the first suspect: if the surface is labelled with one
format and the copy reads it under another, the copy takes values the console
never held. `GEARS_DRAW_NOREINTERP=1` is the control arm and costs one run.

The f32 copies to that same destination cannot be compared yet -- the comparer
refuses format 32 because its decode gives NaN on real data (see the note in
tools/layer_compare.py). Reading out that layout would put the other half of
this destination's story on the table.

### Note (2026-08-11)
THE RELABEL-ON-REFUSAL HYPOTHESIS WAS TESTED AND REFUTED for these two copies.

The chain around them did contain a lie, and it is now fixed: a conversion this
pass REFUSES still relabelled the surface, so the surface claimed a format its
bits were not in and the NEXT conversion converted from a format the data was
never in. On walk_gameplay.gfr the resolve at diag 614 met k_8_8_8_8 -> k_16_16,
which the pass refuses, and the relabel made draw 615 convert
"k_16_16 -> k_2_10_10_10_FLOAT" on data that was still 8888. With the relabel
gated on the conversion's return value, 615 now converts
k_8_8_8_8 -> k_2_10_10_10_FLOAT, which is what its bits are.

AND IT CHANGED THESE COPIES NOT AT ALL: still 0.0869 for both, against the
console's 0.1760 and 0.1804 with the whole pass off. So the darkening is not the
relabel, and it is not the conversion chain being wrong about which format the
data is in -- it is something about the conversions themselves.

What that leaves: the pass converts 6 times on this frame
(k_2_10_10_10->k_8_8_8_8, k_2_10_10_10->k_2_10_10_10_FLOAT,
k_2_10_10_10_FLOAT->k_2_10_10_10). One of those is between the copies and their
source. The next step is a per-conversion arm -- convert all but one -- rather
than the whole-pass switch, because "the pass off is right" and "one conversion
is wrong" look identical from a single number.

### Note (2026-08-11)
NARROWED TO ONE CONVERSION, with a per-pair control arm (GEARS_DRAW_NOCONVERT,
added for this).

f7 copy at diag 639, mean, on walk_gameplay.gfr:

    default (all conversions)                       0.0869
    suppress k_2_10_10_10       -> k_8_8_8_8        0.0869   (no effect)
    suppress k_2_10_10_10       -> k_2_10_10_10_FLOAT 0.0869 (no effect)
    suppress k_2_10_10_10_FLOAT -> k_2_10_10_10     0.1804   <-- the whole of it
    suppress a pair that does not occur (9-9)       0.0869   (negative control)
    whole pass off (GEARS_DRAW_NOREINTERP=1)        0.1804
    THE CONSOLE                                     0.1760

So one conversion -- k_2_10_10_10_FLOAT -> k_2_10_10_10, reading a 7e3 float
surface back as fixed-point 2:10:10:10 -- accounts for the entire difference,
and without it we land within 0.004 of the console. The other two pairs change
nothing, and the pair that does not occur changes nothing and SAYS it matched
none, so the arm is not reporting a difference it cannot make.

## What that does NOT settle

Whether the conversion is wrongly IMPLEMENTED or wrongly TRIGGERED. The console
agreeing with our unconverted values is weak evidence for the second -- if the
console were reinterpreting 7e3 bits as fixed 2:10:10:10 it would not generally
land near the float values -- but "generally" is not a measurement.

The next step is to read RB_COLOR_INFO at that copy: pd.resolveSrcFormat comes
from RB_COLOR_INFO[copy_src_select], and if the surface genuinely is
k_2_10_10_10_FLOAT there then the conversion is firing on a format change that
did not happen and the trigger is the bug. The draw table carries
resolve_dest_fmt but not the SOURCE format; adding it answers this in one run.

### Note (2026-08-11)
THE CONVERSION IS CORRECT, AND CORRECTLY TRIGGERED. Both suspects are now ruled
out by measurement rather than by argument.

TRIGGERED: the draw table now carries resolve_src_fmt. The two f7 copies read
their source as format 10, k_2_10_10_10_AS_10_10_10_10, which stores identically
to k_2_10_10_10 -- while the surface holds k_2_10_10_10_FLOAT, written by draw
615. So a 7e3-to-fixed reinterpretation is exactly what the console's read of
those bits is, and the pass is right to fire.

IMPLEMENTED: the reinterpretation self-test covered four pairs and NOT this one,
which is the shape of every instrument failure here. Adding it:

    k_2_10_10_10_FLOAT -> k_2_10_10_10 (HDR 3.0 as fixed point)
      (3, 3, 3, 1) -> (0.5629883, 0.5629883, 0.5629883, 1)

3.0 in 7e3 is the bits 0x240 -- exponent field 4, mantissa 64, i.e. 1.5 * 2^1 --
and 576/1023 = 0.5630. The shader is exact. 6 of 6 cases pass.

## So the question moves upstream: what does the surface actually hold?

The console's copies land at 0.1760, which is near our UNCONVERTED values
(0.1804) and far from the correctly-converted ones (0.0869). If the conversion
is right and fires at the right moment, then the surface's CONTENTS differ from
the console's before the copy.

The nearest suspect is the approximation the pass already documents: storage
format is per SURFACE, so a NON-BLENDING draw that covers only part of it
relabels the whole thing. Draw 615 is that draw here -- mask 0xf, identity
blend, so the not-read branch relabels to k_2_10_10_10_FLOAT without converting
-- and under the EDRAM sample model a 1X full-screen draw covers 1,280x720 of a
1,280x1,440 grid, which is HALF. The f7 copies are 1X and read only the covered
half, so that does not explain them by itself, but the label is now provably
coarser than the coverage and that is worth measuring before anything else.

### Note (2026-08-11)
THE FIRST NOCONVERT RUN IS VOID -- it was taken against a console frame that
had ONE shadow-casting light where our side had two (catalog #98). Its numbers
(ours 0.0307/0.0595 against the console's 0.1648/0.1649) are not a measurement
of the conversion; they are a measurement of two different scenes, and the
comparer now says so rather than printing the table straight.

Being retaken with the frame window in place. The prediction under test is
unchanged: of the frame's three conversion pairs only 3-2 moves these copies,
and it moves them the whole way.

### Resolution (2026-08-11)
NOT A RENDERER DEFECT. The comparison was decoding the CONSOLE's side wrongly.

`layer_compare.py` applied `copy_dest_endian` to the eight-byte destination
format and to depth and to nothing else. Every k_2_10_10_10 dump in this title
is k8in32, so its dword's four bytes were read in the wrong order -- which does
not shift a value, it scrambles the bit fields. The 2-bit alpha lands in the low
bits and comes out as RED:

    console f7 #0, per channel     mean            zero-fraction
      decoded without the endian   0.009 0.344 0.176   0.969 0.128 0.120
      decoded with it              0.015 0.013 0.012   0.180 0.186 0.194
      ours                         0.018 0.014 0.013   0.159 0.171 0.188

With the endian applied both copies MATCH: ours 0.0147 against the console's
0.0135 and 0.0136, 4.0% of pixels differing by more than 0.1.

What this retracts, from this issue's own notes:
  * "the copies are near-black on our side" -- they are not; both sides are
    near-black there, and that is what the pass writes.
  * the whole hunt for what surface 0x2d0 contains before diag 639.
  * the prediction that suppressing the 3-2 conversion would move them the
    whole way. Measured on an aligned pair: it moves them from 0.0147 to 0.0310
    and BREAKS the three HDR resolves that matched exactly (0.0035 -> 0.0253).
    The conversion is right and is needed.

What SURVIVED, and is worth keeping: the console's own log now shows what it
does at these copies, and it is what our renderer does. At EDRAM base 720, 867
resolves read the surface as k_2_10_10_10_AS_10_10_10_10 while the render
target owning it holds k_2_10_10_10_FLOAT -- a 7e3-to-fixed reinterpretation,
the exact pair our pass converts. The reinterpretation self-test's sixth case
(3.0 as 0x240 -> 576/1023) stands on its own.
