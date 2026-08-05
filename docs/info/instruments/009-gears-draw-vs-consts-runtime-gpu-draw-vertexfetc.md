---
id: I009
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

GEARS_DRAW_VS_CONSTS (runtime/gpu_draw_vertexfetch.cpp)

## Validated by

Run against BOTH classes on courtyard.gfr. POSITIVE: GEARS_DRAW_VS_CONSTS=286,287,288 printed three distinct 16-vec4 constant sets and reported 'matched 3 of 726 draws offered'. NEGATIVE: GEARS_DRAW_VS_CONSTS=99999,foo printed a WARNING -- 'matched 0 of 726 draws offered (diag indices 0..742 this frame); 99999 IS OUT OF RANGE for this frame; foo is not a draw index and was ignored' -- so a mistyped or out-of-frame index cannot be mistaken for a draw that had nothing to show. GEARS_DRAW_VDUMP was rebuilt on the same selection and now reports its own misses too. tools/frame_hashes.sh is byte-identical across all 8 captures before and after, so the instrument changed no picture.

## Known failure modes

(none recorded yet)
