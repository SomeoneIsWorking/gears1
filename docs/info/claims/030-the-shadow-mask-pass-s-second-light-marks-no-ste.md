---
id: C030
kind: claim
status: holds
created: 2026-08-12
tags: render,depth,stencil,mask
depends: runtime/gpu_draw.cpp, runtime/gpu_draw_targets.cpp
---

## Claim

The shadow-mask pass's second light marks NO stencil on our side because the 30,876-vertex marking draw writes stencil only on ZPASS (REPLACE with ref 1) and tests GEQUAL against a scene depth buffer that the shadow atlas has overwritten -- one host depth image shared across every RB_DEPTH_INFO.depth_base. It is the shared depth image, not the projection, the atlas or the stencil test.

## Evidence

One frame, both shader-aimed dumps armed (I035). After the 30,876-vertex marking draw the stencil contains not one sample at 0x01 -- the only value REPLACE-with-ref-1 can write -- and is bit-identical to the buffer before it; the surface after each of the two shading draws that follow is bit-identical to the one before. The 36-vertex depth-fail box in the same frame's second loop marks 194,202 samples at 0x01, so the marking machinery is sound. Control arm: with GEARS_DRAW_SPLIT_DEPTH=1 the same shader's marking draws leave 258,325 / 51,191 / 41,203 samples at 0x01 and no 0xff anywhere.

## What would falsify it

a paired capture with GEARS_DRAW_SPLIT_DEPTH=1 in which mask #1 is still confined to a corner, or in which the console's own marking draw also writes no stencil
