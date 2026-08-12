---
id: C036
kind: claim
status: holds
created: 2026-08-12
tags: 
---

## Claim

Our clear-quad draws rasterise if and only if they target depth base 0. In one frame, 55 draws of shader 760aacf6212e632c at base 0x0 produce all 58,604 fragments, while 23 at 0x5a0, 4 at 0x2d0 and 1 at 0x400 produce none. Pipeline state is identical (vte_cntl 0x300, clip_cntl 0x10000) across both groups; the vertex data is not. These are pre-transformed window-space quads and the dead ones carry a fourth component of 0 where the live ones carry 1, so treating it as a clip-space w makes every primitive degenerate.

## Evidence

scratch/clearfix/split0/draws.tsv and its GEARS_DRAW_VDUMP_VS dump; GEARS_DRAW_SPLIT_DEPTH=1 does not change it

## What would falsify it

a frame in which a clear draw at a non-zero depth base produces fragments, or one in which a dead draw's fourth component is not 0
