---
id: 95
title: Two k_2_10_10_10 copies of the scene surface are near-black where the console has content
status: open
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
