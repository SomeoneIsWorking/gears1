---
id: C030
kind: claim
status: holds
created: 2026-08-12
tags: render,depth,stencil,mask
depends: runtime/gpu_draw_resolve_decode.cpp
reconfirmed: 2026-08-12
verified_at: 2026-08-12 01:55:55
---

## Claim

The shadow-mask pass's second light marks NO stencil on our side because the 30,876-vertex marking draw writes stencil only on ZPASS (REPLACE with ref 1) and tests GEQUAL against a scene depth buffer that the shadow atlas has overwritten -- one host depth image shared across every RB_DEPTH_INFO.depth_base. It is the shared depth image, not the projection, the atlas or the stencil test.

## Evidence

One frame, both shader-aimed dumps armed (I035). After the 30,876-vertex marking draw the stencil contains not one sample at 0x01 -- the only value REPLACE-with-ref-1 can write -- and is bit-identical to the buffer before it; the surface after each of the two shading draws that follow is bit-identical to the one before. The 36-vertex depth-fail box in the same frame's second loop marks 194,202 samples at 0x01, so the marking machinery is sound. Control arm: with GEARS_DRAW_SPLIT_DEPTH=1 the same shader's marking draws leave 258,325 / 51,191 / 41,203 samples at 0x01 and no 0xff anywhere.

## What would falsify it

a paired capture with GEARS_DRAW_SPLIT_DEPTH=1 in which mask #1 is still confined to a corner, or in which the console's own marking draw also writes no stencil

## Re-confirmed 2026-08-12

CONFIRMED end to end on the fixed split-depth build (scratch/splitfix, our side, gameplay frame). The first mask loop's marking draws now write stencil -- 252,120 / 52,907 / 43,939 samples at 0x01 -- and the shading draws that follow take EXACTLY those fragment counts: draw 1017 shades 52,907 and draw 1019 shades 43,939, against ZERO for the same draws on the shared-depth build. Mark count and consumed count agreeing to the sample is the strongest form this claim could take. The mask that pass resolves goes from a flat 1.0000 (nothing shadowed) to 0.8206 against the console's 0.8513, and the shadow atlas's own depth resolve from 0.0209 to 0.5523. So the second light's marks were indeed being lost to a depth buffer another EDRAM base had overwritten.
