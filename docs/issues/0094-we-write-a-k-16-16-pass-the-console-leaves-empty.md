---
id: 94
title: We write a k_16_16 pass the console leaves empty, and render almost nothing in two k_2_10_10_10 passes
status: open
symptom: layer_compare: srcC2D0 f25 ours 0.2502 vs console 0.0000; srcC2D0 f7 ours 0.0146 vs console 0.1760
tags: oracle,layer-compare,resolve,msaa
created: 2026-08-11
updated: 2026-08-11
---

## What was measured

`tools/layer_compare.py` learned to decode k_2_10_10_10 and k_16_16
destinations, which judged three passes that had been REFUSED. Both results are
new, on the SP_Prison_P paired capture (`scratch/layercap_vp`):

    pass                     ours     console   mean |d|
    srcC2D0 1280x720 f7  #0  0.0146   0.1760    0.173
    srcC2D0 1280x720 f7  #1  0.0146   0.1761    0.173
    srcC2D0 1280x720 f25 #0  0.2502   0.0000    0.250

So in one direction we render almost nothing where the console has real
content, and in the other we write content into a destination the console
leaves entirely empty.

## A prediction about the f25 one, worth checking before investigating it

Under GEARS_DRAW_MSAA (the EDRAM sample model), that destination's equivalent
in the replay frame -- 0xcb91000, fed by the 1X copy at diag 614 -- goes from
49.9% non-zero to 0%. The reason is known: before that copy, surface 0x2d0 is
written only by the EDRAM colour/depth aliasing pass, and the fill that pass
reads (diag 613) covers a 640x360 QUADRANT at pixel scale and all 1,280x720
samples of its surface under the model. At pixel scale the other three quarters
keep the scene depth and alias into colour as 75% non-zero garbage; under the
model the whole surface is depth 0 + stencil 0, which unpacks to black.

If that is right, the sample model turns this row from DIFFER into a match, and
this entry is a symptom of catalog #91 rather than a defect of its own. The
paired capture with the model on is what settles it -- do not investigate the
f25 pass before reading that run.

The f7 rows are NOT explained by anything so far.
