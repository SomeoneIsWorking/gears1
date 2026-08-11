---
id: I035
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

GEARS_DRAW_DEPTH_DUMP_PS / GEARS_DRAW_SURFACE_DUMP_PS: the depth+stencil and surface dumps aimed by PIXEL SHADER instead of by diag draw index, armable together so marks and the shading they gate come from ONE capture

## Validated by

Both arms run on a real frame. Positive: the hash of the shadow-mask shader dumps 4 of 10 (depth, colour mask 0) and 6 of 10 (surface, colour mask non-zero) and reports both counts. Negative: GEARS_DRAW_DEPTH_DUMP_PS=deadbeefdeadbeef prints 'NO dump. The frame ran 0 draw(s) of that pixel shader, 0 of them matching the colour-mask rule, over 2 draws reaching diag index 0. That is the draw this asked for did not happen, NOT the depth buffer was empty' -- i.e. it carries both denominators and refuses to look like a clean result. The reason it exists: a diag index is not stable between runs of this title, so every earlier per-draw reading had to chain a bbox from one run against a bbox from another, which twice produced a conclusion neither run supported.

## Known failure modes

(none recorded yet)
